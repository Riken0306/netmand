/*
 * netmand — logger.c
 *
 * Ring buffer + write-through sink dispatch.  See logger.h for the design
 * rationale (why the ring is not a queue, and why this module carries
 * file-scope state when nothing else in netmand does).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "logger.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* log_level_t -> syslog priority.  Indexed by level, which log_vwrite()
 * has already bounds-checked against LOG_LEVEL_NONE. */
static const int syslog_priority[] = {
    LOG_DEBUG,      /* LOG_LEVEL_DEBUG */
    LOG_INFO,       /* LOG_LEVEL_INFO  */
    LOG_WARNING,    /* LOG_LEVEL_WARN  */
    LOG_ERR         /* LOG_LEVEL_ERROR */
};

/* ====================================================================
 * State
 * ================================================================= */

typedef struct {
    int    fd;                      /* -1 when the file sink is inactive  */
    char   path[LOG_PATH_MAX];
    size_t max_bytes;               /* rotate once size would exceed this */
    size_t size;                    /* bytes currently in the live file   */
} log_file_sink_t;

static struct {
    log_level_t     level;
    uint32_t        sinks;          /* LOG_SINK_* bitmask                 */
    log_ring_t      ring;

    log_file_sink_t file;

    log_callback_fn callback;
    void           *callback_user;

    bool            syslog_open;
} lg = {
    LOG_LEVEL_INFO, 0u, { { { 0, 0, { 0 } } }, 0u },
    { -1, { 0 }, LOG_FILE_MAX_DEFAULT, 0u },
    NULL, NULL, false
};

/* ====================================================================
 * Small helpers
 * ================================================================= */

static uint64_t monotonic_us(void)
{
    struct timespec ts;

    /*
     * CLOCK_MONOTONIC, not CLOCK_REALTIME (AD-6).  On a box with no RTC the
     * wall clock jumps by decades the moment NTP first syncs, which would
     * reorder a log file and make every lease-timing calculation derived
     * from these stamps nonsense.  Timestamps therefore read as seconds
     * since boot, kernel-dmesg style.
     */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;

    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
}

/* write(2) until the whole buffer is out.  Returns bytes written. */
static size_t write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;      /* a broken sink must not take the daemon with it */
        }
        if (n == 0)
            break;
        off += (size_t)n;
    }

    return off;
}

const char *log_level_name(int level)
{
    switch (level) {
    case LOG_LEVEL_DEBUG: return "DEBUG";
    case LOG_LEVEL_INFO:  return "INFO";
    case LOG_LEVEL_WARN:  return "WARN";
    case LOG_LEVEL_ERROR: return "ERROR";
    case LOG_LEVEL_NONE:  return "NONE";
    default:              return "?";
    }
}

int log_level_from_name(const char *name)
{
    static const struct { const char *name; int level; } map[] = {
        { "debug", LOG_LEVEL_DEBUG },
        { "info",  LOG_LEVEL_INFO  },
        { "warn",  LOG_LEVEL_WARN  },
        { "error", LOG_LEVEL_ERROR },
        { "none",  LOG_LEVEL_NONE  }
    };
    size_t i;

    if (!name)
        return -1;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcasecmp(name, map[i].name) == 0)
            return map[i].level;
    }

    return -1;
}

/* ====================================================================
 * File sink
 *
 * NOTE: the target's filesystem is flash.  Every rotation rewrites a whole
 * generation, so the size cap is a wear knob, not just a disk-space knob —
 * a 64 KiB cap on a chatty debug build will churn the device far harder
 * than the default 1 MiB.  One .old generation only, for the same reason.
 * ================================================================= */

static void file_sink_close(void)
{
    if (lg.file.fd >= 0)
        close(lg.file.fd);

    lg.file.fd   = -1;
    lg.file.size = 0;
    lg.file.path[0] = '\0';
}

static int file_sink_open(const char *path)
{
    struct stat st;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);

    if (fd < 0)
        return -1;

    lg.file.fd   = fd;
    lg.file.size = (fstat(fd, &st) == 0 && st.st_size > 0)
                       ? (size_t)st.st_size : 0u;

    return 0;
}

/* Rename the live file to "<path>.old" and start a fresh one. */
static void file_sink_rotate(void)
{
    char old[LOG_PATH_MAX + 8];

    if (snprintf(old, sizeof(old), "%s.old", lg.file.path) >= (int)sizeof(old))
        return;     /* path too long to rotate; keep appending */

    close(lg.file.fd);
    lg.file.fd = -1;

    /* A failed rename still leaves us needing a live fd, so reopen either
     * way — worst case we keep appending to an oversized file. */
    (void)rename(lg.file.path, old);

    if (file_sink_open(lg.file.path) < 0)
        lg.sinks &= ~LOG_SINK_FILE;
}

static void file_sink_write(const char *line, size_t len)
{
    if (lg.file.fd < 0)
        return;

    if (lg.file.max_bytes > 0 && lg.file.size + len > lg.file.max_bytes) {
        file_sink_rotate();
        if (lg.file.fd < 0)
            return;
    }

    lg.file.size += write_all(lg.file.fd, line, len);
}

/* ====================================================================
 * Lifecycle
 * ================================================================= */

void log_init(log_level_t level)
{
    log_shutdown();
    lg.level = level;
}

void log_shutdown(void)
{
    file_sink_close();

    if (lg.syslog_open) {
        closelog();
        lg.syslog_open = false;
    }

    lg.sinks          = 0u;
    lg.callback       = NULL;
    lg.callback_user  = NULL;
    lg.file.max_bytes = LOG_FILE_MAX_DEFAULT;
    lg.ring.head      = 0u;

    memset(lg.ring.entries, 0, sizeof(lg.ring.entries));
}

void log_set_level(log_level_t level)
{
    lg.level = level;
}

log_level_t log_get_level(void)
{
    return lg.level;
}

uint32_t log_active_sinks(void)
{
    return lg.sinks;
}

int log_add_sink(uint32_t sink, const log_sink_cfg_t *cfg)
{
    switch (sink) {
    case LOG_SINK_STDERR:
        break;

    case LOG_SINK_SYSLOG:
        if (lg.syslog_open)
            closelog();
        openlog((cfg && cfg->ident) ? cfg->ident : "netmand",
                LOG_PID | LOG_NDELAY, LOG_DAEMON);
        lg.syslog_open = true;
        break;

    case LOG_SINK_FILE:
        if (!cfg || !cfg->path || cfg->path[0] == '\0') {
            errno = EINVAL;
            return -1;
        }
        if (strlen(cfg->path) >= LOG_PATH_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }

        file_sink_close();      /* re-adding the sink may change the path */

        if (file_sink_open(cfg->path) < 0) {
            lg.sinks &= ~LOG_SINK_FILE;
            return -1;
        }

        /* file_sink_open() does not know the path; record it for rotation. */
        strncpy(lg.file.path, cfg->path, LOG_PATH_MAX - 1);
        lg.file.path[LOG_PATH_MAX - 1] = '\0';
        lg.file.max_bytes = (cfg->max_bytes > 0) ? cfg->max_bytes
                                                 : LOG_FILE_MAX_DEFAULT;
        break;

    case LOG_SINK_CALLBACK:
        if (!cfg || !cfg->callback) {
            errno = EINVAL;
            return -1;
        }
        lg.callback      = cfg->callback;
        lg.callback_user = cfg->user;
        break;

    default:
        errno = EINVAL;     /* not a single valid LOG_SINK_* bit */
        return -1;
    }

    lg.sinks |= sink;
    return 0;
}

void log_remove_sink(uint32_t mask)
{
    if (mask & LOG_SINK_FILE)
        file_sink_close();

    if ((mask & LOG_SINK_SYSLOG) && lg.syslog_open) {
        closelog();
        lg.syslog_open = false;
    }

    if (mask & LOG_SINK_CALLBACK) {
        lg.callback      = NULL;
        lg.callback_user = NULL;
    }

    lg.sinks &= ~mask;
}

/* ====================================================================
 * Writing
 * ================================================================= */

void log_write(log_level_t level, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    log_vwrite(level, fmt, ap);
    va_end(ap);
}

void log_vwrite(log_level_t level, const char *fmt, va_list ap)
{
    log_entry_t *entry;
    char         line[LOG_MSG_MAX + 64];
    int          n;

    if (level < lg.level || level >= LOG_LEVEL_NONE || !fmt)
        return;

    /*
     * Format straight into the ring slot: one vsnprintf, no scratch copy,
     * no allocation.  vsnprintf() truncates and NUL-terminates by itself,
     * so an over-long message costs a lost tail, never an overflow.
     */
    entry = &lg.ring.entries[lg.ring.head & (LOG_RING_SIZE - 1u)];
    entry->level        = (int)level;
    entry->timestamp_us = monotonic_us();
    (void)vsnprintf(entry->msg, sizeof(entry->msg), fmt, ap);
    lg.ring.head++;

    if (lg.sinks == 0u)
        return;

    /* One rendered line, shared by the fd-backed sinks. */
    n = snprintf(line, sizeof(line), "[%6" PRIu64 ".%06" PRIu64 "] %-5s %s\n",
                 entry->timestamp_us / 1000000u,
                 entry->timestamp_us % 1000000u,
                 log_level_name(entry->level),
                 entry->msg);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(line))
        n = (int)sizeof(line) - 1;

    if (lg.sinks & LOG_SINK_STDERR)
        (void)write_all(STDERR_FILENO, line, (size_t)n);

    if (lg.sinks & LOG_SINK_FILE)
        file_sink_write(line, (size_t)n);

    if (lg.sinks & LOG_SINK_SYSLOG)
        syslog(syslog_priority[level], "%s", entry->msg);

    if ((lg.sinks & LOG_SINK_CALLBACK) && lg.callback)
        lg.callback(lg.callback_user, entry->level, entry->timestamp_us,
                    entry->msg);
}

/* ====================================================================
 * Ring readout
 * ================================================================= */

size_t log_ring_dump(log_entry_t *out, size_t max)
{
    size_t   held, count, i;
    uint32_t start;

    if (!out || max == 0)
        return 0;

    held  = (lg.ring.head < LOG_RING_SIZE) ? lg.ring.head : LOG_RING_SIZE;
    count = (max < held) ? max : held;

    /* Oldest first: walk back `count` slots from the write cursor. */
    start = lg.ring.head - (uint32_t)count;

    for (i = 0; i < count; i++)
        out[i] = lg.ring.entries[(start + (uint32_t)i) & (LOG_RING_SIZE - 1u)];

    return count;
}

uint32_t log_ring_head(void)
{
    return lg.ring.head;
}
