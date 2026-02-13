#include "log.h"

#if _WIN32
# include <windows.h>
#endif
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL_atomic.h>
#include <libavutil/log.h>

#define SC_SESSION_LOG_MAX_LENGTH (1 << 20) // 1 MiB
#define SC_SESSION_LOG_INITIAL_CAPACITY 4096

static SDL_SpinLock sc_session_log_lock = 0;
static char *sc_session_log;
static size_t sc_session_log_len;
static size_t sc_session_log_cap;

static bool
sc_session_log_reserve(size_t required) {
    if (required <= sc_session_log_cap) {
        return true;
    }

    size_t new_cap = sc_session_log_cap
        ? sc_session_log_cap
        : SC_SESSION_LOG_INITIAL_CAPACITY;
    while (new_cap < required) {
        new_cap *= 2;
    }

    char *new_buf = realloc(sc_session_log, new_cap);
    if (!new_buf) {
        return false;
    }

    sc_session_log = new_buf;
    sc_session_log_cap = new_cap;
    return true;
}

static void
sc_session_log_trim(size_t append_len) {
    size_t required = sc_session_log_len + append_len;
    if (required <= SC_SESSION_LOG_MAX_LENGTH) {
        return;
    }

    size_t drop = required - SC_SESSION_LOG_MAX_LENGTH;
    if (drop >= sc_session_log_len) {
        sc_session_log_len = 0;
        return;
    }

    // Trim full lines when possible.
    while (drop < sc_session_log_len && sc_session_log[drop] != '\n') {
        ++drop;
    }
    if (drop < sc_session_log_len) {
        ++drop; // include '\n'
    }

    memmove(sc_session_log, &sc_session_log[drop], sc_session_log_len - drop);
    sc_session_log_len -= drop;
}

static void
sc_session_log_append(const char *prio_name, const char *message) {
    size_t prio_len = strlen(prio_name);
    size_t message_len = strlen(message);
    size_t line_len = prio_len + 2 + message_len + 1; // "PRIO: MSG\n"

    SDL_AtomicLock(&sc_session_log_lock);

    sc_session_log_trim(line_len);

    size_t required = sc_session_log_len + line_len + 1;
    bool ok = sc_session_log_reserve(required);
    if (ok) {
        char *dst = &sc_session_log[sc_session_log_len];
        memcpy(dst, prio_name, prio_len);
        memcpy(&dst[prio_len], ": ", 2);
        memcpy(&dst[prio_len + 2], message, message_len);
        dst[prio_len + 2 + message_len] = '\n';
        sc_session_log_len += line_len;
        sc_session_log[sc_session_log_len] = '\0';
    }

    SDL_AtomicUnlock(&sc_session_log_lock);
}

static SDL_LogPriority
log_level_sc_to_sdl(enum sc_log_level level) {
    switch (level) {
        case SC_LOG_LEVEL_VERBOSE:
            return SDL_LOG_PRIORITY_VERBOSE;
        case SC_LOG_LEVEL_DEBUG:
            return SDL_LOG_PRIORITY_DEBUG;
        case SC_LOG_LEVEL_INFO:
            return SDL_LOG_PRIORITY_INFO;
        case SC_LOG_LEVEL_WARN:
            return SDL_LOG_PRIORITY_WARN;
        case SC_LOG_LEVEL_ERROR:
            return SDL_LOG_PRIORITY_ERROR;
        default:
            assert(!"unexpected log level");
            return SDL_LOG_PRIORITY_INFO;
    }
}

static enum sc_log_level
log_level_sdl_to_sc(SDL_LogPriority priority) {
    switch (priority) {
        case SDL_LOG_PRIORITY_VERBOSE:
            return SC_LOG_LEVEL_VERBOSE;
        case SDL_LOG_PRIORITY_DEBUG:
            return SC_LOG_LEVEL_DEBUG;
        case SDL_LOG_PRIORITY_INFO:
            return SC_LOG_LEVEL_INFO;
        case SDL_LOG_PRIORITY_WARN:
            return SC_LOG_LEVEL_WARN;
        case SDL_LOG_PRIORITY_ERROR:
            return SC_LOG_LEVEL_ERROR;
        default:
            assert(!"unexpected log level");
            return SC_LOG_LEVEL_INFO;
    }
}

void
sc_set_log_level(enum sc_log_level level) {
    SDL_LogPriority sdl_log = log_level_sc_to_sdl(level);
    SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, sdl_log);
    SDL_LogSetPriority(SDL_LOG_CATEGORY_CUSTOM, sdl_log);
}

enum sc_log_level
sc_get_log_level(void) {
    SDL_LogPriority sdl_log = SDL_LogGetPriority(SDL_LOG_CATEGORY_APPLICATION);
    return log_level_sdl_to_sc(sdl_log);
}

void
sc_log(enum sc_log_level level, const char *fmt, ...) {
    SDL_LogPriority sdl_level = log_level_sc_to_sdl(level);

    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION, sdl_level, fmt, ap);
    va_end(ap);
}

#ifdef _WIN32
bool
sc_log_windows_error(const char *prefix, int error) {
    assert(prefix);

    char *message;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM;
    DWORD lang_id = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    int ret =
        FormatMessage(flags, NULL, error, lang_id, (char *) &message, 0, NULL);
    if (ret <= 0) {
        return false;
    }

    // Note: message already contains a trailing '\n'
    LOGE("%s: [%d] %s", prefix, error, message);
    LocalFree(message);
    return true;
}
#endif

static SDL_LogPriority
sdl_priority_from_av_level(int level) {
    switch (level) {
        case AV_LOG_PANIC:
        case AV_LOG_FATAL:
            return SDL_LOG_PRIORITY_CRITICAL;
        case AV_LOG_ERROR:
            return SDL_LOG_PRIORITY_ERROR;
        case AV_LOG_WARNING:
            return SDL_LOG_PRIORITY_WARN;
        case AV_LOG_INFO:
            return SDL_LOG_PRIORITY_INFO;
    }
    // do not forward others, which are too verbose
    return 0;
}

static void
sc_av_log_callback(void *avcl, int level, const char *fmt, va_list vl) {
    (void) avcl;
    SDL_LogPriority priority = sdl_priority_from_av_level(level);
    if (priority == 0) {
        return;
    }

    size_t fmt_len = strlen(fmt);
    char *local_fmt = malloc(fmt_len + 10);
    if (!local_fmt) {
        LOG_OOM();
        return;
    }
    memcpy(local_fmt, "[FFmpeg] ", 9); // do not write the final '\0'
    memcpy(local_fmt + 9, fmt, fmt_len + 1); // include '\0'
    SDL_LogMessageV(SDL_LOG_CATEGORY_CUSTOM, priority, local_fmt, vl);
    free(local_fmt);
}

static const char *const sc_sdl_log_priority_names[SDL_NUM_LOG_PRIORITIES] = {
    [SDL_LOG_PRIORITY_VERBOSE] = "VERBOSE",
    [SDL_LOG_PRIORITY_DEBUG] = "DEBUG",
    [SDL_LOG_PRIORITY_INFO] = "INFO",
    [SDL_LOG_PRIORITY_WARN] = "WARN",
    [SDL_LOG_PRIORITY_ERROR] = "ERROR",
    [SDL_LOG_PRIORITY_CRITICAL] = "CRITICAL",
};

static void SDLCALL
sc_sdl_log_print(void *userdata, int category, SDL_LogPriority priority,
                 const char *message) {
    (void) userdata;
    (void) category;

    FILE *out = priority < SDL_LOG_PRIORITY_WARN ? stdout : stderr;
    assert(priority < SDL_NUM_LOG_PRIORITIES);
    const char *prio_name = sc_sdl_log_priority_names[priority];
    sc_session_log_append(prio_name, message);
    fprintf(out, "%s: %s\n", prio_name, message);
}

void
sc_log_configure(void) {
    SDL_LogSetOutputFunction(sc_sdl_log_print, NULL);
    // Redirect FFmpeg logs to SDL logs
    av_log_set_callback(sc_av_log_callback);
}

char *
sc_log_get_session_text(void) {
    SDL_AtomicLock(&sc_session_log_lock);

    size_t len = sc_session_log_len;
    char *copy = malloc(len + 1);
    if (copy) {
        if (len && sc_session_log) {
            memcpy(copy, sc_session_log, len);
        }
        copy[len] = '\0';
    }

    SDL_AtomicUnlock(&sc_session_log_lock);
    return copy;
}
