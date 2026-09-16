/*
 * netmand — logger.h
 *
 * Levelled logging with a fixed-size retrospective ring buffer and a set
 * of write-through sinks (syslog, stderr, file, callback).
 *
 * WHAT THE RING IS FOR
 * --------------------
 * The ring is *not* a queue.  The daemon is single-threaded and every sink
 * is written synchronously inside log_write(), so by the time log_write()
 * returns the message has already reached syslog / stderr / the file.
 *
 * The ring exists so that the last LOG_RING_SIZE messages can be dumped on
 * demand — `netmandctl log` (Step 10) asking a daemon that was configured
 * to log only to syslog, on a box where syslogd is not running.  Nothing
 * drains it, nothing flushes it, and it needs no flush thread.
 *
 * NO ALLOCATION, NO BLOCKING
 * --------------------------
 * log_write() formats with vsnprintf() straight into the ring slot and then
 * dispatches.  It never calls malloc().  The only calls that can block are
 * the sinks' own write(2) / syslog(3) — which is why sinks are plain fds and
 * the file sink is opened O_APPEND rather than buffered through stdio.
 *
 * SINGLE INSTANCE
 * ---------------
 * Logger state is file-scope in logger.c rather than hanging off
 * netmand_ctx_t.  This is the one deliberate exception to main.h's "no
 * global state" rule: the emit macros take no context argument, and
 * failure paths that run before a context exists still need to log.  The
 * daemon is single-threaded, so the state needs no locking.
 */

#ifndef NETMAND_LOGGER_H
#define NETMAND_LOGGER_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* --------------------------------------------------------------------
 * Compile-time sizing
 *
 * 64 x 128 B = 8 KiB of BSS.  This is an embedded target: scrollback that
 * costs 64 KiB of RAM is not scrollback, it is a memory leak with a nice
 * name.  LOG_RING_SIZE must stay a power of two — the index is computed by
 * masking, not by modulo.
 * ----------------------------------------------------------------- */

#define LOG_RING_SIZE          64u
#define LOG_MSG_MAX            128u
#define LOG_PATH_MAX           128u    /* matches daemon_config.log_file  */
#define LOG_FILE_MAX_DEFAULT   (1024u * 1024u)   /* 1 MiB, then rotate    */

/* LOG_RING_SIZE must be a power of two; this fails to compile if it is not. */
typedef char log_ring_size_must_be_power_of_two[
    (LOG_RING_SIZE != 0u && (LOG_RING_SIZE & (LOG_RING_SIZE - 1u)) == 0u)
        ? 1 : -1];

/* --------------------------------------------------------------------
 * Levels
 * ----------------------------------------------------------------- */

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_NONE          /* threshold only: silences every message */
} log_level_t;

/* --------------------------------------------------------------------
 * Sinks — a bitmask, so struct daemon_config can carry it verbatim
 * ----------------------------------------------------------------- */

#define LOG_SINK_SYSLOG    (1u << 0)
#define LOG_SINK_STDERR    (1u << 1)
#define LOG_SINK_FILE      (1u << 2)
#define LOG_SINK_CALLBACK  (1u << 3)
#define LOG_SINK_ALL       (LOG_SINK_SYSLOG | LOG_SINK_STDERR | \
                            LOG_SINK_FILE   | LOG_SINK_CALLBACK)

typedef void (*log_callback_fn)(void *user, int level, uint64_t timestamp_us,
                                const char *msg);

/*
 * Per-sink configuration.  Only the fields belonging to the sink being
 * added are read; NULL is accepted for sinks that need no configuration
 * (stderr, and syslog with the default ident).
 */
typedef struct {
    /* LOG_SINK_FILE */
    const char     *path;        /* log file path                          */
    size_t          max_bytes;   /* rotate above this; 0 = 1 MiB default   */

    /* LOG_SINK_SYSLOG */
    const char     *ident;       /* NULL = "netmand"                       */

    /* LOG_SINK_CALLBACK */
    log_callback_fn callback;
    void           *user;
} log_sink_cfg_t;

/* --------------------------------------------------------------------
 * Ring buffer
 * ----------------------------------------------------------------- */

typedef struct {
    int      level;
    uint64_t timestamp_us;       /* CLOCK_MONOTONIC — see AD-6            */
    char     msg[LOG_MSG_MAX];
} log_entry_t;

typedef struct {
    log_entry_t entries[LOG_RING_SIZE];
    uint32_t    head;            /* monotonic write counter;
                                    index = head & (LOG_RING_SIZE - 1)    */
} log_ring_t;

/* --------------------------------------------------------------------
 * API
 * ----------------------------------------------------------------- */

/*
 * Reset the logger: empty ring, no sinks, threshold set to `level`.
 * Safe to call again to start over (tests do); any open sinks are closed
 * first.  Call log_add_sink() afterwards — after log_init() alone the
 * daemon logs to the ring and nowhere else.
 */
void log_init(log_level_t level);

/*
 * Close the file sink and syslog, forget the callback, empty the ring.
 */
void log_shutdown(void);

void        log_set_level(log_level_t level);
log_level_t log_get_level(void);

/*
 * Enable one sink.  `sink` is a single LOG_SINK_* bit, not a mask.
 * Adding a sink that is already active replaces its configuration (the
 * file sink reopens, possibly on a different path).
 * Returns 0 on success, -1 on error (bad argument, or the file would not
 * open — errno is left set by the failing call).
 */
int  log_add_sink(uint32_t sink, const log_sink_cfg_t *cfg);

/*
 * Disable every sink whose bit is set in `mask`.
 */
void log_remove_sink(uint32_t mask);

/*
 * Which sinks are currently active, as a LOG_SINK_* mask.
 */
uint32_t log_active_sinks(void);

/* Emit a message.  Below-threshold messages are dropped before they reach
 * the ring, so the ring holds exactly what was logged. */
#if defined(__GNUC__)
#  define LOG_PRINTF_FMT(fmt_idx, arg_idx) \
       __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#  define LOG_PRINTF_FMT(fmt_idx, arg_idx)
#endif

void log_write(log_level_t level, const char *fmt, ...) LOG_PRINTF_FMT(2, 3);
void log_vwrite(log_level_t level, const char *fmt, va_list ap);

/*
 * The macros every other module should use.  Lowercase because they must not
 * collide with <syslog.h> — see "WHY THE MACROS ARE LOWERCASE" above.  They
 * take no context argument by design — see "SINGLE INSTANCE" above.
 */
#define log_debug(...) log_write(LOG_LEVEL_DEBUG, __VA_ARGS__)
#define log_info(...)  log_write(LOG_LEVEL_INFO,  __VA_ARGS__)
#define log_warn(...)  log_write(LOG_LEVEL_WARN,  __VA_ARGS__)
#define log_error(...) log_write(LOG_LEVEL_ERROR, __VA_ARGS__)

/*
 * Copy up to `max` ring entries into `out`, oldest first.  Returns the
 * number copied.  This is what `netmandctl log` (Step 10) serves.
 */
size_t   log_ring_dump(log_entry_t *out, size_t max);

/*
 * Total messages ever written (not the number currently held).  Exposed so
 * callers — and the unit tests — can tell "the ring wrapped" from "the ring
 * is full".
 */
uint32_t log_ring_head(void);

/* "DEBUG" / "INFO" / "WARN" / "ERROR", or "?" for a level out of range. */
const char *log_level_name(int level);

/*
 * Parse "debug" / "info" / "warn" / "error" (case-insensitive) as written
 * in netmand.conf.  Returns -1 if the name is not recognised.  The mapping
 * lives here rather than in the config parser because the enum does.
 */
int log_level_from_name(const char *name);

#endif /* NETMAND_LOGGER_H */
