#ifndef SC_FIGMA_BRIDGE_H
#define SC_FIGMA_BRIDGE_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/net.h"
#include "util/thread.h"

struct sc_figma_bridge {
    sc_thread thread;
    sc_mutex mutex;

    sc_socket server_socket;
    bool running;
    uint16_t port;

    uint64_t sequence;
    uint8_t *png_data;
    size_t png_size;
    uint16_t width;
    uint16_t height;
};

bool
sc_figma_bridge_init(struct sc_figma_bridge *bridge, uint16_t port);

bool
sc_figma_bridge_start(struct sc_figma_bridge *bridge);

void
sc_figma_bridge_stop(struct sc_figma_bridge *bridge);

void
sc_figma_bridge_destroy(struct sc_figma_bridge *bridge);

bool
sc_figma_bridge_publish_png(struct sc_figma_bridge *bridge,
                            const uint8_t *png_data, size_t png_size,
                            uint16_t width, uint16_t height);

uint16_t
sc_figma_bridge_get_port(const struct sc_figma_bridge *bridge);

#endif
