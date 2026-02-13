#include "figma_bridge.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"

#define SC_FIGMA_BRIDGE_BACKLOG 4

struct sc_figma_bridge_snapshot {
    uint64_t sequence;
    uint8_t *png_data;
    size_t png_size;
    uint16_t width;
    uint16_t height;
};

static void
sc_figma_bridge_snapshot_destroy(struct sc_figma_bridge_snapshot *snapshot) {
    free(snapshot->png_data);
}

static char *
sc_figma_bridge_base64_encode(const uint8_t *data, size_t len,
                              size_t *out_len) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    size_t b64_len = ((len + 2) / 3) * 4;
    char *out = malloc(b64_len + 1);
    if (!out) {
        LOG_OOM();
        return NULL;
    }

    size_t i = 0;
    size_t j = 0;
    while (i + 3 <= len) {
        uint32_t value = ((uint32_t) data[i] << 16)
                       | ((uint32_t) data[i + 1] << 8)
                       | (uint32_t) data[i + 2];
        out[j++] = table[(value >> 18) & 0x3F];
        out[j++] = table[(value >> 12) & 0x3F];
        out[j++] = table[(value >> 6) & 0x3F];
        out[j++] = table[value & 0x3F];
        i += 3;
    }

    if (i < len) {
        uint32_t value = (uint32_t) data[i] << 16;
        out[j++] = table[(value >> 18) & 0x3F];
        if (i + 1 < len) {
            value |= (uint32_t) data[i + 1] << 8;
            out[j++] = table[(value >> 12) & 0x3F];
            out[j++] = table[(value >> 6) & 0x3F];
            out[j++] = '=';
        } else {
            out[j++] = table[(value >> 12) & 0x3F];
            out[j++] = '=';
            out[j++] = '=';
        }
    }

    assert(j == b64_len);
    out[b64_len] = '\0';
    *out_len = b64_len;
    return out;
}

static bool
sc_figma_bridge_send_headers(sc_socket client, int code, const char *status,
                             const char *content_type, size_t body_len) {
    char headers[512];
    int r = snprintf(headers, sizeof(headers),
                     "HTTP/1.1 %d %s\r\n"
                     "Connection: close\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
                     "Access-Control-Allow-Headers: *\r\n"
                     "Cache-Control: no-store\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %" SC_PRIsizet "\r\n"
                     "\r\n",
                     code, status, content_type, body_len);
    if (r < 0 || (size_t) r >= sizeof(headers)) {
        LOGW("Could not format Figma Bridge HTTP headers");
        return false;
    }

    return net_send_all(client, headers, (size_t) r) == r;
}

static void
sc_figma_bridge_send_response(sc_socket client, int code, const char *status,
                              const char *content_type, const char *body) {
    size_t body_len = body ? strlen(body) : 0;
    if (!sc_figma_bridge_send_headers(client, code, status, content_type,
                                      body_len)) {
        return;
    }

    if (!body_len) {
        return;
    }

    ssize_t r = net_send_all(client, body, body_len);
    if (r < 0 || (size_t) r != body_len) {
        LOGW("Could not write Figma Bridge HTTP response body");
    }
}

static bool
sc_figma_bridge_parse_after_query(const char *query, uint64_t *after) {
    *after = 0;
    if (!query || !*query) {
        return true;
    }

    const char *p = query;
    while (*p) {
        const char *sep = strchr(p, '&');
        size_t token_len = sep ? (size_t) (sep - p) : strlen(p);
        if (token_len > 6 && !strncmp(p, "after=", 6)) {
            const char *value = p + 6;
            size_t value_len = token_len - 6;
            if (!value_len) {
                return false;
            }
            for (size_t i = 0; i < value_len; ++i) {
                if (!isdigit((unsigned char) value[i])) {
                    return false;
                }
            }

            char tmp[32];
            if (value_len >= sizeof(tmp)) {
                return false;
            }
            memcpy(tmp, value, value_len);
            tmp[value_len] = '\0';

            char *endptr;
            unsigned long long parsed = strtoull(tmp, &endptr, 10);
            if (*endptr) {
                return false;
            }
            *after = (uint64_t) parsed;
            return true;
        }

        if (!sep) {
            break;
        }
        p = sep + 1;
    }

    return true;
}

static bool
sc_figma_bridge_snapshot_newer_than(struct sc_figma_bridge *bridge,
                                    uint64_t after,
                                    struct sc_figma_bridge_snapshot *snapshot) {
    sc_mutex_lock(&bridge->mutex);
    bool available = bridge->sequence > after && bridge->png_data
                  && bridge->png_size;
    if (!available) {
        sc_mutex_unlock(&bridge->mutex);
        return false;
    }

    uint8_t *data = malloc(bridge->png_size);
    if (!data) {
        sc_mutex_unlock(&bridge->mutex);
        LOG_OOM();
        return false;
    }

    memcpy(data, bridge->png_data, bridge->png_size);
    snapshot->sequence = bridge->sequence;
    snapshot->png_data = data;
    snapshot->png_size = bridge->png_size;
    snapshot->width = bridge->width;
    snapshot->height = bridge->height;
    sc_mutex_unlock(&bridge->mutex);

    return true;
}

static void
sc_figma_bridge_respond_latest(struct sc_figma_bridge *bridge, sc_socket client,
                               const char *query) {
    uint64_t after;
    bool ok = sc_figma_bridge_parse_after_query(query, &after);
    if (!ok) {
        sc_figma_bridge_send_response(client, 400, "Bad Request",
                                      "text/plain; charset=utf-8",
                                      "Invalid query\n");
        return;
    }

    struct sc_figma_bridge_snapshot snapshot = {
        .sequence = 0,
        .png_data = NULL,
        .png_size = 0,
        .width = 0,
        .height = 0,
    };
    bool has_snapshot =
        sc_figma_bridge_snapshot_newer_than(bridge, after, &snapshot);
    if (!has_snapshot) {
        sc_figma_bridge_send_response(client, 204, "No Content",
                                      "application/json", NULL);
        return;
    }

    size_t b64_len;
    char *png_b64 = sc_figma_bridge_base64_encode(snapshot.png_data,
                                                   snapshot.png_size, &b64_len);
    sc_figma_bridge_snapshot_destroy(&snapshot);
    if (!png_b64) {
        sc_figma_bridge_send_response(client, 500, "Internal Server Error",
                                      "text/plain; charset=utf-8",
                                      "Out of memory\n");
        return;
    }

    int prefix_len =
        snprintf(NULL, 0,
                 "{\"seq\":%" PRIu64 ",\"width\":%u,\"height\":%u,\"png_base64\":\"",
                 snapshot.sequence, (unsigned) snapshot.width,
                 (unsigned) snapshot.height);
    if (prefix_len < 0) {
        free(png_b64);
        sc_figma_bridge_send_response(client, 500, "Internal Server Error",
                                      "text/plain; charset=utf-8",
                                      "Formatting error\n");
        return;
    }

    size_t body_len = (size_t) prefix_len + b64_len + 2; // "\"}"
    char *body = malloc(body_len + 1);
    if (!body) {
        free(png_b64);
        LOG_OOM();
        sc_figma_bridge_send_response(client, 500, "Internal Server Error",
                                      "text/plain; charset=utf-8",
                                      "Out of memory\n");
        return;
    }

    int written = snprintf(body, body_len + 1,
                           "{\"seq\":%" PRIu64 ",\"width\":%u,\"height\":%u,"
                           "\"png_base64\":\"",
                           snapshot.sequence, (unsigned) snapshot.width,
                           (unsigned) snapshot.height);
    assert(written == prefix_len);

    memcpy(&body[written], png_b64, b64_len);
    body[written + b64_len] = '"';
    body[written + b64_len + 1] = '}';
    body[body_len] = '\0';

    free(png_b64);

    if (!sc_figma_bridge_send_headers(client, 200, "OK",
                                      "application/json; charset=utf-8",
                                      body_len)) {
        free(body);
        return;
    }

    ssize_t r = net_send_all(client, body, body_len);
    free(body);
    if (r < 0 || (size_t) r != body_len) {
        LOGW("Could not write Figma Bridge JSON payload");
    }
}

static void
sc_figma_bridge_handle_client(struct sc_figma_bridge *bridge, sc_socket client) {
    char req[4096];
    ssize_t r = net_recv(client, req, sizeof(req) - 1);
    if (r <= 0) {
        return;
    }
    req[r] = '\0';

    char *line_end = strstr(req, "\r\n");
    if (!line_end) {
        line_end = strchr(req, '\n');
    }
    if (!line_end) {
        sc_figma_bridge_send_response(client, 400, "Bad Request",
                                      "text/plain; charset=utf-8",
                                      "Malformed request\n");
        return;
    }
    *line_end = '\0';

    char method[8];
    char uri[1024];
    if (sscanf(req, "%7s %1023s", method, uri) != 2) {
        sc_figma_bridge_send_response(client, 400, "Bad Request",
                                      "text/plain; charset=utf-8",
                                      "Malformed request line\n");
        return;
    }

    if (!strcmp(method, "OPTIONS")) {
        sc_figma_bridge_send_response(client, 204, "No Content",
                                      "text/plain; charset=utf-8", NULL);
        return;
    }

    if (strcmp(method, "GET")) {
        sc_figma_bridge_send_response(client, 405, "Method Not Allowed",
                                      "text/plain; charset=utf-8",
                                      "Only GET is supported\n");
        return;
    }

    char *query = strchr(uri, '?');
    if (query) {
        *query = '\0';
        ++query;
    }

    if (!strcmp(uri, "/scrcpy-bridge/health")) {
        sc_figma_bridge_send_response(client, 200, "OK",
                                      "text/plain; charset=utf-8",
                                      "ok\n");
        return;
    }

    if (!strcmp(uri, "/scrcpy-bridge/latest")) {
        sc_figma_bridge_respond_latest(bridge, client, query);
        return;
    }

    sc_figma_bridge_send_response(client, 404, "Not Found",
                                  "text/plain; charset=utf-8", "Not found\n");
}

static int
sc_figma_bridge_run(void *userdata) {
    struct sc_figma_bridge *bridge = userdata;

    LOGI("Figma Bridge listening on http://127.0.0.1:%u/scrcpy-bridge/latest",
         (unsigned) bridge->port);

    for (;;) {
        sc_socket client = net_accept(bridge->server_socket);
        if (client == SC_SOCKET_NONE) {
            sc_mutex_lock(&bridge->mutex);
            bool running = bridge->running;
            sc_mutex_unlock(&bridge->mutex);
            if (!running) {
                break;
            }
            continue;
        }

        sc_figma_bridge_handle_client(bridge, client);
        if (!net_close(client)) {
            LOGW("Could not close Figma Bridge client socket");
        }
    }

    LOGD("Figma Bridge stopped");
    return 0;
}

bool
sc_figma_bridge_init(struct sc_figma_bridge *bridge, uint16_t port) {
    bool ok = sc_mutex_init(&bridge->mutex);
    if (!ok) {
        return false;
    }

    bridge->running = false;
    bridge->port = port;
    bridge->sequence = 0;
    bridge->png_data = NULL;
    bridge->png_size = 0;
    bridge->width = 0;
    bridge->height = 0;

    sc_socket server_socket = net_socket();
    if (server_socket == SC_SOCKET_NONE) {
        sc_mutex_destroy(&bridge->mutex);
        return false;
    }

    ok = net_listen(server_socket, IPV4_LOCALHOST, port, SC_FIGMA_BRIDGE_BACKLOG);
    if (!ok) {
        net_close(server_socket);
        sc_mutex_destroy(&bridge->mutex);
        return false;
    }

    bridge->server_socket = server_socket;
    return true;
}

bool
sc_figma_bridge_start(struct sc_figma_bridge *bridge) {
    sc_mutex_lock(&bridge->mutex);
    assert(!bridge->running);
    bridge->running = true;
    sc_mutex_unlock(&bridge->mutex);

    bool ok = sc_thread_create(&bridge->thread, sc_figma_bridge_run,
                               "scrcpy-figma-bridge", bridge);
    if (!ok) {
        sc_mutex_lock(&bridge->mutex);
        bridge->running = false;
        sc_mutex_unlock(&bridge->mutex);
        LOGE("Could not start Figma Bridge thread");
        return false;
    }

    return true;
}

void
sc_figma_bridge_stop(struct sc_figma_bridge *bridge) {
    sc_mutex_lock(&bridge->mutex);
    if (!bridge->running) {
        sc_mutex_unlock(&bridge->mutex);
        return;
    }
    bridge->running = false;
    sc_mutex_unlock(&bridge->mutex);

    net_interrupt(bridge->server_socket);
    sc_thread_join(&bridge->thread, NULL);
}

void
sc_figma_bridge_destroy(struct sc_figma_bridge *bridge) {
    net_close(bridge->server_socket);
    bridge->server_socket = SC_SOCKET_NONE;

    sc_mutex_lock(&bridge->mutex);
    free(bridge->png_data);
    bridge->png_data = NULL;
    bridge->png_size = 0;
    bridge->width = 0;
    bridge->height = 0;
    sc_mutex_unlock(&bridge->mutex);

    sc_mutex_destroy(&bridge->mutex);
}

bool
sc_figma_bridge_publish_png(struct sc_figma_bridge *bridge,
                            const uint8_t *png_data, size_t png_size,
                            uint16_t width, uint16_t height) {
    assert(png_data);
    assert(png_size);

    uint8_t *copy = malloc(png_size);
    if (!copy) {
        LOG_OOM();
        return false;
    }
    memcpy(copy, png_data, png_size);

    sc_mutex_lock(&bridge->mutex);
    free(bridge->png_data);
    bridge->png_data = copy;
    bridge->png_size = png_size;
    bridge->width = width;
    bridge->height = height;
    ++bridge->sequence;
    uint64_t seq = bridge->sequence;
    sc_mutex_unlock(&bridge->mutex);

    LOGI("Figma Bridge queued screenshot #%" PRIu64 " (%" SC_PRIsizet " bytes)",
         seq, png_size);
    return true;
}

uint16_t
sc_figma_bridge_get_port(const struct sc_figma_bridge *bridge) {
    return bridge->port;
}
