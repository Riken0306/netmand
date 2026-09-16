/*
 * netmand — tests/test_logger.c
 *
 * Unit tests for src/logger.  No test framework: a CHECK macro, a counter,
 * and a non-zero exit status is all `make tests` and the CI sanitiser build
 * need.  Every test calls log_init() first, so cases cannot leak sink state
 * into one another.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "logger/logger.h"

/*
 * Included deliberately, and deliberately *after* logger.h: the logger's
 * macros are lowercase precisely so that a translation unit can use both
 * headers.  If someone renames log_info() back to LOG_INFO(), this include
 * turns the regression into a compile error right here instead of a puzzling
 * -Werror failure in whichever module first needs syslog.
 */
#include <syslog.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ====================================================================
 * Harness
 * ================================================================= */

static int checks;
static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                    \
    } while (0)

#define RUN(fn)                                                              \
    do {                                                                     \
        int before = failures;                                               \
        printf("  %-24s", #fn);                                              \
        fflush(stdout);                                                      \
        fn();                                                                \
        printf("%s\n", (failures == before) ? "ok" : "FAILED");              \
    } while (0)

/* ====================================================================
 * Helpers
 * ================================================================= */

/* Create a unique temp path and remove the file mkstemp() made, so the
 * logger opens it itself (and so rotation tests start from nothing). */
static int temp_path(char *buf, size_t len)
{
    int fd;

    if (snprintf(buf, len, "/tmp/netmand_test_XXXXXX") >= (int)len)
        return -1;

    fd = mkstemp(buf);
    if (fd < 0)
        return -1;

    close(fd);
    unlink(buf);
    return 0;
}

/* Read a whole file into `buf`, NUL-terminated.  Returns bytes read, -1. */
static ssize_t slurp(const char *path, char *buf, size_t len)
{
    ssize_t n;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return -1;

    n = read(fd, buf, len - 1);
    close(fd);

    if (n < 0)
        return -1;

    buf[n] = '\0';
    return n;
}

/* Capture state for the stderr redirection used by test_stderr_sink. */
typedef struct {
    int  saved_fd;
    char path[64];
} stderr_capture_t;

static int stderr_capture_begin(stderr_capture_t *cap)
{
    int fd;

    if (temp_path(cap->path, sizeof(cap->path)) < 0)
        return -1;

    fd = open(cap->path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;

    cap->saved_fd = dup(STDERR_FILENO);
    if (cap->saved_fd < 0) {
        close(fd);
        return -1;
    }

    fflush(stderr);
    dup2(fd, STDERR_FILENO);
    close(fd);
    return 0;
}

static void stderr_capture_end(stderr_capture_t *cap)
{
    fflush(stderr);
    dup2(cap->saved_fd, STDERR_FILENO);
    close(cap->saved_fd);
}

/* A callback sink that records what it was handed. */
typedef struct {
    int  calls;
    int  last_level;
    char last_msg[LOG_MSG_MAX];
    int  user_seen;
} probe_t;

static void probe_cb(void *user, int level, uint64_t ts_us, const char *msg)
{
    probe_t *p = user;

    (void)ts_us;

    if (!p)
        return;

    p->calls++;
    p->last_level = level;
    p->user_seen  = 1;
    strncpy(p->last_msg, msg, sizeof(p->last_msg) - 1);
    p->last_msg[sizeof(p->last_msg) - 1] = '\0';
}

/* ====================================================================
 * Tests
 * ================================================================= */

/* Messages below the threshold never reach the ring or the sinks. */
static void test_log_levels(void)
{
    probe_t probe;
    log_sink_cfg_t cfg;

    memset(&probe, 0, sizeof(probe));
    memset(&cfg, 0, sizeof(cfg));
    cfg.callback = probe_cb;
    cfg.user     = &probe;

    log_init(LOG_LEVEL_WARN);
    CHECK(log_add_sink(LOG_SINK_CALLBACK, &cfg) == 0);

    log_write(LOG_LEVEL_DEBUG, "debug");
    log_write(LOG_LEVEL_INFO,  "info");
    log_write(LOG_LEVEL_WARN,  "warn");
    log_write(LOG_LEVEL_ERROR, "error");

    CHECK(probe.calls == 2);
    CHECK(log_ring_head() == 2);       /* dropped before the ring, not after */

    /* Raising the threshold at runtime takes effect immediately. */
    log_set_level(LOG_LEVEL_DEBUG);
    CHECK(log_get_level() == LOG_LEVEL_DEBUG);
    log_write(LOG_LEVEL_DEBUG, "now visible");
    CHECK(probe.calls == 3);

    /* LOG_LEVEL_NONE silences everything, including ERROR. */
    log_set_level(LOG_LEVEL_NONE);
    log_write(LOG_LEVEL_ERROR, "silenced");
    CHECK(probe.calls == 3);

    log_shutdown();
}

/* The ring wraps, evicts oldest-first, and head keeps counting. */
static void test_ring_wrap(void)
{
    log_entry_t dump[LOG_RING_SIZE];
    unsigned    total = LOG_RING_SIZE + 5u;
    unsigned    i;
    size_t      n;

    log_init(LOG_LEVEL_DEBUG);

    for (i = 0; i < total; i++)
        log_write(LOG_LEVEL_INFO, "msg %u", i);

    /* head is a monotonic write counter, not an index — it does not wrap. */
    CHECK(log_ring_head() == total);

    n = log_ring_dump(dump, LOG_RING_SIZE);
    CHECK(n == LOG_RING_SIZE);

    /* Oldest surviving entry is #5; the first five were evicted. */
    CHECK(strcmp(dump[0].msg, "msg 5") == 0);
    CHECK(strcmp(dump[n - 1].msg, "msg 68") == 0);

    /* A short dump returns the newest entries, still oldest-first. */
    n = log_ring_dump(dump, 3);
    CHECK(n == 3);
    CHECK(strcmp(dump[0].msg, "msg 66") == 0);
    CHECK(strcmp(dump[2].msg, "msg 68") == 0);

    /* Before the ring fills, dump returns only what is held. */
    log_init(LOG_LEVEL_DEBUG);
    log_write(LOG_LEVEL_INFO, "only one");
    CHECK(log_ring_dump(dump, LOG_RING_SIZE) == 1);

    log_shutdown();
}

/* An over-long message truncates inside the slot; nothing overflows. */
static void test_truncation(void)
{
    log_entry_t dump[1];
    char        huge[512];
    probe_t     probe;
    log_sink_cfg_t cfg;

    memset(huge, 'A', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';

    memset(&probe, 0, sizeof(probe));
    memset(&cfg, 0, sizeof(cfg));
    cfg.callback = probe_cb;
    cfg.user     = &probe;

    log_init(LOG_LEVEL_DEBUG);
    CHECK(log_add_sink(LOG_SINK_CALLBACK, &cfg) == 0);

    log_write(LOG_LEVEL_INFO, "%s", huge);

    CHECK(log_ring_dump(dump, 1) == 1);
    CHECK(strlen(dump[0].msg) == LOG_MSG_MAX - 1);   /* NUL-terminated */
    CHECK(dump[0].msg[LOG_MSG_MAX - 1] == '\0');
    CHECK(strlen(probe.last_msg) == LOG_MSG_MAX - 1);

    /* A format expansion that overflows the rendered line is also safe. */
    log_write(LOG_LEVEL_INFO, "%s%s", huge, huge);
    CHECK(log_ring_dump(dump, 1) == 1);
    CHECK(strlen(dump[0].msg) == LOG_MSG_MAX - 1);

    log_shutdown();
}

/* Output reaches fd 2. */
static void test_stderr_sink(void)
{
    stderr_capture_t cap;
    char buf[1024];

    log_init(LOG_LEVEL_DEBUG);
    CHECK(log_add_sink(LOG_SINK_STDERR, NULL) == 0);

    if (stderr_capture_begin(&cap) < 0) {
        CHECK(0 && "stderr capture failed");
        log_shutdown();
        return;
    }
    log_write(LOG_LEVEL_WARN, "to stderr %d", 42);
    stderr_capture_end(&cap);

    CHECK(slurp(cap.path, buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, "to stderr 42") != NULL);
    CHECK(strstr(buf, "WARN") != NULL);

    unlink(cap.path);
    log_shutdown();
}

/* Output reaches the configured file, with level and message intact. */
static void test_file_sink(void)
{
    char path[64];
    char buf[1024];
    log_sink_cfg_t cfg;

    CHECK(temp_path(path, sizeof(path)) == 0);

    memset(&cfg, 0, sizeof(cfg));
    cfg.path = path;

    log_init(LOG_LEVEL_DEBUG);
    CHECK(log_add_sink(LOG_SINK_FILE, &cfg) == 0);
    CHECK((log_active_sinks() & LOG_SINK_FILE) != 0);

    log_write(LOG_LEVEL_ERROR, "file line %s", "one");
    log_write(LOG_LEVEL_INFO,  "file line two");

    CHECK(slurp(path, buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, "file line one") != NULL);
    CHECK(strstr(buf, "file line two") != NULL);
    CHECK(strstr(buf, "ERROR") != NULL);

    /* A path that cannot be opened is reported, not silently swallowed. */
    cfg.path = "/nonexistent-dir-netmand/x.log";
    CHECK(log_add_sink(LOG_SINK_FILE, &cfg) == -1);
    CHECK((log_active_sinks() & LOG_SINK_FILE) == 0);

    /* A NULL path is rejected rather than crashing. */
    memset(&cfg, 0, sizeof(cfg));
    CHECK(log_add_sink(LOG_SINK_FILE, &cfg) == -1);
    CHECK(log_add_sink(LOG_SINK_FILE, NULL) == -1);

    log_shutdown();
    unlink(path);
}

/* Exceeding the cap rotates to .old and logging continues. */
static void test_file_rotation(void)
{
    char path[64];
    char old[80];
    char buf[4096];
    struct stat st;
    log_sink_cfg_t cfg;
    int  i;

    CHECK(temp_path(path, sizeof(path)) == 0);
    snprintf(old, sizeof(old), "%s.old", path);

    memset(&cfg, 0, sizeof(cfg));
    cfg.path      = path;
    cfg.max_bytes = 512;

    log_init(LOG_LEVEL_DEBUG);
    CHECK(log_add_sink(LOG_SINK_FILE, &cfg) == 0);

    /* Each line is ~50 bytes, so 40 lines is several rotations. */
    for (i = 0; i < 40; i++)
        log_write(LOG_LEVEL_INFO, "rotation filler line %02d", i);

    /* The previous generation was kept... */
    CHECK(stat(old, &st) == 0);
    CHECK(st.st_size > 0);
    CHECK((size_t)st.st_size <= 512u);

    /* ...the live file is under the cap and still being written... */
    CHECK(stat(path, &st) == 0);
    CHECK((size_t)st.st_size <= 512u);

    /* ...and the newest message is in the live file, not the .old one. */
    CHECK(slurp(path, buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, "rotation filler line 39") != NULL);

    /* Only one .old generation exists — there is no .old.old. */
    {
        char older[96];
        snprintf(older, sizeof(older), "%s.old.old", path);
        CHECK(stat(older, &st) != 0);
    }

    log_shutdown();
    unlink(path);
    unlink(old);
}

/* The callback receives the level, the message, and its own user pointer. */
static void test_callback_sink(void)
{
    probe_t probe;
    log_sink_cfg_t cfg;

    memset(&probe, 0, sizeof(probe));
    memset(&cfg, 0, sizeof(cfg));
    cfg.callback = probe_cb;
    cfg.user     = &probe;

    log_init(LOG_LEVEL_DEBUG);
    CHECK(log_add_sink(LOG_SINK_CALLBACK, &cfg) == 0);

    log_write(LOG_LEVEL_ERROR, "callback %s %d", "says", 7);

    CHECK(probe.calls == 1);
    CHECK(probe.last_level == LOG_LEVEL_ERROR);
    CHECK(strcmp(probe.last_msg, "callback says 7") == 0);
    CHECK(probe.user_seen == 1);

    /* A callback sink with no function is an error, not a NULL call. */
    memset(&cfg, 0, sizeof(cfg));
    CHECK(log_add_sink(LOG_SINK_CALLBACK, &cfg) == -1);

    /* Removal actually stops delivery. */
    log_remove_sink(LOG_SINK_CALLBACK);
    log_write(LOG_LEVEL_ERROR, "after removal");
    CHECK(probe.calls == 1);
    CHECK(log_ring_head() == 2);      /* still recorded in the ring */

    log_shutdown();
}

/* One log_write() reaches every active sink. */
static void test_multiple_sinks(void)
{
    stderr_capture_t cap;
    probe_t probe;
    char path[64];
    char buf[1024];
    log_sink_cfg_t cfg;

    CHECK(temp_path(path, sizeof(path)) == 0);
    memset(&probe, 0, sizeof(probe));

    log_init(LOG_LEVEL_DEBUG);

    memset(&cfg, 0, sizeof(cfg));
    cfg.path = path;
    CHECK(log_add_sink(LOG_SINK_FILE, &cfg) == 0);

    memset(&cfg, 0, sizeof(cfg));
    cfg.callback = probe_cb;
    cfg.user     = &probe;
    CHECK(log_add_sink(LOG_SINK_CALLBACK, &cfg) == 0);

    CHECK(log_add_sink(LOG_SINK_STDERR, NULL) == 0);
    CHECK(log_active_sinks() ==
          (LOG_SINK_FILE | LOG_SINK_CALLBACK | LOG_SINK_STDERR));

    if (stderr_capture_begin(&cap) < 0) {
        CHECK(0 && "stderr capture failed");
        log_shutdown();
        unlink(path);
        return;
    }
    log_write(LOG_LEVEL_INFO, "broadcast");
    stderr_capture_end(&cap);

    CHECK(probe.calls == 1);
    CHECK(strcmp(probe.last_msg, "broadcast") == 0);

    CHECK(slurp(path, buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, "broadcast") != NULL);

    CHECK(slurp(cap.path, buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, "broadcast") != NULL);

    CHECK(log_ring_head() == 1);      /* one write, one ring entry */

    unlink(cap.path);
    log_shutdown();
    unlink(path);
}

/*
 * The LOG_* macros map to the levels they name.  This also pins down the
 * syslog coexistence: the file includes <syslog.h>, so syslog's LOG_INFO
 * (an int) and the logger's log_info() (a macro) are both in scope here.
 */
static void test_macros(void)
{
    log_entry_t dump[4];
    probe_t probe;
    log_sink_cfg_t cfg;

    memset(&probe, 0, sizeof(probe));
    memset(&cfg, 0, sizeof(cfg));
    cfg.callback = probe_cb;
    cfg.user     = &probe;

    log_init(LOG_LEVEL_DEBUG);
    CHECK(log_add_sink(LOG_SINK_CALLBACK, &cfg) == 0);

    log_debug("a %d", 1);
    log_info("b");
    log_warn("c");
    log_error("d");

    CHECK(probe.calls == 4);
    CHECK(log_ring_dump(dump, 4) == 4);
    CHECK(dump[0].level == LOG_LEVEL_DEBUG);
    CHECK(dump[1].level == LOG_LEVEL_INFO);
    CHECK(dump[2].level == LOG_LEVEL_WARN);
    CHECK(dump[3].level == LOG_LEVEL_ERROR);
    CHECK(strcmp(dump[0].msg, "a 1") == 0);

    /* syslog's own constants are untouched and still usable. */
    CHECK(LOG_INFO != LOG_ERR);

    log_shutdown();
}

/* Level names round-trip, as config.c (Step 3) will need them to. */
static void test_level_names(void)
{
    CHECK(strcmp(log_level_name(LOG_LEVEL_DEBUG), "DEBUG") == 0);
    CHECK(strcmp(log_level_name(LOG_LEVEL_ERROR), "ERROR") == 0);
    CHECK(strcmp(log_level_name(999), "?") == 0);

    CHECK(log_level_from_name("info")  == LOG_LEVEL_INFO);
    CHECK(log_level_from_name("WARN")  == LOG_LEVEL_WARN);
    CHECK(log_level_from_name("Error") == LOG_LEVEL_ERROR);
    CHECK(log_level_from_name("bogus") == -1);
    CHECK(log_level_from_name(NULL)    == -1);
}

/* log_shutdown() leaves the logger inert but safe to call into. */
static void test_shutdown_is_inert(void)
{
    log_init(LOG_LEVEL_DEBUG);
    log_write(LOG_LEVEL_INFO, "before");
    log_shutdown();

    CHECK(log_active_sinks() == 0);
    CHECK(log_ring_head() == 0);

    log_write(LOG_LEVEL_ERROR, "after shutdown");   /* must not crash */
    log_shutdown();                                 /* idempotent      */
}

/* ====================================================================
 * main
 * ================================================================= */

int main(void)
{
    printf("test_logger\n");

    RUN(test_log_levels);
    RUN(test_ring_wrap);
    RUN(test_truncation);
    RUN(test_stderr_sink);
    RUN(test_file_sink);
    RUN(test_file_rotation);
    RUN(test_callback_sink);
    RUN(test_multiple_sinks);
    RUN(test_macros);
    RUN(test_level_names);
    RUN(test_shutdown_is_inert);

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
