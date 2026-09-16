/*
 * netmand — main.c
 *
 * Daemon entry point.  Parses CLI arguments, sets up signal handling via
 * signalfd, writes a PID file, and enters an epoll-based event loop.
 *
 * Signals are delivered through the epoll loop (not async handlers) to
 * avoid all async-signal-safety issues.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* for signalfd, epoll_create1 */
#endif

#include "main.h"
#include "priv.h"

#include "logger/logger.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>

/* ====================================================================
 * Forward declarations (file-local)
 * ================================================================= */

static void print_usage(const char *prog);
static int  parse_args(netmand_ctx_t *ctx, int argc, char **argv);
static int  write_pid_file(netmand_ctx_t *ctx);
static void remove_pid_file(netmand_ctx_t *ctx);
static int  setup_signals(netmand_ctx_t *ctx);
static void handle_signal(netmand_ctx_t *ctx, int fd, uint32_t events);
static int  daemonize(netmand_ctx_t *ctx);
static void daemon_ready(netmand_ctx_t *ctx, int status);

/* ====================================================================
 * Event loop implementation
 * ================================================================= */

int event_loop_add(netmand_ctx_t *ctx, epoll_handler_t *handler,
                   uint32_t events)
{
    struct epoll_event ev = {
        .events = events,
        .data.ptr = handler,
    };
    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, handler->fd, &ev) < 0) {
        log_error("epoll_ctl ADD fd=%d: %s", handler->fd, strerror(errno));
        return -1;
    }
    return 0;
}

int event_loop_remove(netmand_ctx_t *ctx, int fd)
{
    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        log_error("epoll_ctl DEL fd=%d: %s", fd, strerror(errno));
        return -1;
    }
    return 0;
}

void event_loop_run(netmand_ctx_t *ctx)
{
    struct epoll_event events[NETMAND_MAX_EPOLL_EVS];

    log_info("entering event loop");

    while (ctx->running) {
        int nfds = epoll_wait(ctx->epoll_fd, events, NETMAND_MAX_EPOLL_EVS, -1);
        if (nfds < 0) {
            if (errno == EINTR)
                continue;
            log_error("epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            epoll_handler_t *handler = events[i].data.ptr;
            if (handler && handler->callback) {
                handler->callback(ctx, handler->fd, events[i].events);
            }
        }

        /* Handle config reload outside the event dispatch to avoid
         * re-entrancy issues. */
        if (ctx->reload_pending) {
            ctx->reload_pending = false;
            log_info("config reload requested "
                     "(stub — config parser not yet implemented)");
            /* TODO: call config_reload(ctx) once config.c exists */
        }
    }

    log_info("event loop exited");
}

/* ====================================================================
 * Signal handling via signalfd
 * ================================================================= */

static int setup_signals(netmand_ctx_t *ctx)
{
    sigset_t mask;

    /*
     * Block SIGTERM, SIGINT, SIGHUP so they are delivered to our
     * signalfd instead of being handled asynchronously.
     */
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGHUP);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        log_error("sigprocmask: %s", strerror(errno));
        return -1;
    }

    ctx->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (ctx->signal_fd < 0) {
        log_error("signalfd: %s", strerror(errno));
        return -1;
    }

    /* Register the signalfd with the epoll loop. */
    ctx->signal_handler.fd       = ctx->signal_fd;
    ctx->signal_handler.callback = handle_signal;

    if (event_loop_add(ctx, &ctx->signal_handler, EPOLLIN) < 0) {
        close(ctx->signal_fd);
        ctx->signal_fd = -1;
        return -1;
    }

    return 0;
}

static void handle_signal(netmand_ctx_t *ctx, int fd, uint32_t events)
{
    (void)events;
    struct signalfd_siginfo si;
    ssize_t n;

    for (;;) {
        n = read(fd, &si, sizeof(si));
        if (n != (ssize_t)sizeof(si))
            break;

        switch (si.ssi_signo) {
        case SIGTERM:
            /* fall through */
        case SIGINT:
            log_info("received signal %u, shutting down", si.ssi_signo);
            ctx->running = false;
            break;

        case SIGHUP:
            log_info("received SIGHUP, scheduling reload");
            ctx->reload_pending = true;
            break;

        default:
            log_warn("unexpected signal %u", si.ssi_signo);
            break;
        }
    }
}

/* ====================================================================
 * PID file management
 * ================================================================= */

static int write_pid_file(netmand_ctx_t *ctx)
{
    /* Check for a stale PID file. */
    FILE *f = fopen(ctx->pid_path, "r");
    if (f) {
        pid_t old_pid = 0;
        if (fscanf(f, "%d", &old_pid) == 1 && old_pid > 0) {
            /* Check if process is still running. */
            if (kill(old_pid, 0) == 0) {
                log_error("already running (pid %d)", old_pid);
                fclose(f);
                return -1;
            }
            /* Stale PID file, safe to overwrite. */
            log_warn("removing stale PID file (pid %d)", old_pid);
        }
        fclose(f);
    }

    f = fopen(ctx->pid_path, "w");
    if (!f) {
        log_error("cannot write PID file %s: %s", ctx->pid_path,
                  strerror(errno));
        return -1;
    }
    fprintf(f, "%d\n", getpid());
    fclose(f);

    /* From here on the file is ours, and only we may unlink it. */
    ctx->pid_file_owned = true;

    return 0;
}

static void remove_pid_file(netmand_ctx_t *ctx)
{
    /*
     * Never unlink a PID file we did not write: a second instance that
     * exits because the first one is already running must leave the
     * running daemon's PID file alone, or the init script can no longer
     * stop it.
     */
    if (!ctx->pid_file_owned)
        return;

    unlink(ctx->pid_path);
    ctx->pid_file_owned = false;
}

/* ====================================================================
 * Daemonize — fork, setsid, and report the startup verdict to the parent
 * ================================================================= */

/*
 * Fork into the background.
 *
 * The parent does NOT exit at the fork.  It blocks until the child reports,
 * over a pipe, whether startup actually succeeded, and then exits with that
 * status.  Without this the parent always exits 0 — it forks long before the
 * child reaches the PID file or capability drop — so an init script's
 * `start)` cannot tell a daemon that came up from one that died on the spot,
 * and a boot where netmand failed looks exactly like a boot where it did not.
 *
 * stdout and stderr are deliberately left attached here.  Everything that
 * can still fail (PID file, capabilities) has to be able to say so on the
 * terminal that launched us; daemon_ready() detaches them afterwards.
 *
 * The parent's read is blocking.  Startup does no blocking I/O, so it either
 * completes or the child dies and the read sees EOF — but anything added
 * before daemon_ready() in a later step must keep that true, or a failure
 * there will wedge the caller instead of failing it.
 */
static int daemonize(netmand_ctx_t *ctx)
{
    int   fds[2];
    pid_t pid;

    if (pipe2(fds, O_CLOEXEC) < 0) {
        log_error("pipe2: %s", strerror(errno));
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        log_error("fork: %s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    if (pid > 0) {
        /* Parent: adopt the child's verdict as our exit status. */
        unsigned char status;
        ssize_t       n;

        close(fds[1]);

        do {
            n = read(fds[0], &status, 1);
        } while (n < 0 && errno == EINTR);

        close(fds[0]);

        /* EOF means the child exited without reporting.  It logged the
         * reason to the stderr it still shared with us. */
        _exit((n == 1) ? (int)status : EXIT_FAILURE);
    }

    /* --- Child --- */
    close(fds[0]);
    ctx->ready_fd = fds[1];

    if (setsid() < 0) {
        log_error("setsid: %s", strerror(errno));
        return -1;
    }

    /* stdin has no one to read from, whatever -s says. */
    {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (devnull != STDIN_FILENO)
                close(devnull);
        }
    }

    /* Working directory to / so we don't hold any mount busy. */
    if (chdir("/") < 0)
        log_warn("chdir(\"/\"): %s", strerror(errno));

    return 0;
}

/*
 * Tell the parent how startup went, then let go of the terminal.
 *
 * Idempotent and safe in the foreground, where ready_fd is -1: the cleanup
 * path calls it with EXIT_FAILURE, which does nothing if the success verdict
 * has already gone out.
 */
static void daemon_ready(netmand_ctx_t *ctx, int status)
{
    unsigned char byte = (unsigned char)status;
    ssize_t       n;

    if (ctx->ready_fd < 0)
        return;

    do {
        n = write(ctx->ready_fd, &byte, 1);
    } while (n < 0 && errno == EINTR);

    close(ctx->ready_fd);
    ctx->ready_fd = -1;

    if (status != EXIT_SUCCESS)
        return;     /* about to exit; leave stdio alone for the last logs */

    /*
     * Startup is done and the launcher has been told, so the terminal is no
     * longer ours to hold.  stdout/stderr go to /dev/null — which also stops
     * a stray printf() from anywhere in the process reaching the console —
     * unless -s asked to keep them, the way to watch a detached daemon on an
     * embedded board until the config parser (Step 3) turns on the syslog
     * and file sinks.
     */
    if (ctx->keep_stdio)
        return;

    {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
    }
}

/* ====================================================================
 * CLI argument parsing
 * ================================================================= */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  -c <path>   Path to configuration file (default: %s)\n"
        "  -f          Run in foreground (don't daemonize)\n"
        "  -s          Keep stdout/stderr after startup instead of sending\n"
        "              them to /dev/null (embedded: the debug UART).\n"
        "              Startup errors are shown either way.\n"
        "  -p <path>   PID file path (default: %s)\n"
        "  -v          Print version and exit\n"
        "  -h          Print this help\n",
        prog, NETMAND_DEFAULT_CONF, NETMAND_PID_FILE);
}

static int parse_args(netmand_ctx_t *ctx, int argc, char **argv)
{
    int opt;

    /* Defaults */
    ctx->conf_path  = NETMAND_DEFAULT_CONF;
    ctx->pid_path   = NETMAND_PID_FILE;
    ctx->foreground = false;
    ctx->keep_stdio = false;

    while ((opt = getopt(argc, argv, "c:fsp:vh")) != -1) {
        switch (opt) {
        case 'c':
            ctx->conf_path = optarg;
            break;
        case 'f':
            ctx->foreground = true;
            break;
        case 's':
            ctx->keep_stdio = true;
            break;
        case 'p':
            ctx->pid_path = optarg;
            break;
        case 'v':
            fprintf(stdout, "netmand %s\n", NETMAND_VERSION);
            exit(EXIT_SUCCESS);
        case 'h':
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    return 0;
}

/* ====================================================================
 * main
 * ================================================================= */

int main(int argc, char **argv)
{
    int rc = EXIT_FAILURE;

    /* --- Zero-initialize context --- */
    netmand_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.running    = true;
    ctx.epoll_fd   = -1;
    ctx.signal_fd  = -1;
    ctx.ready_fd   = -1;

    /*
     * A dead reader must never kill the daemon: the readiness pipe below is
     * the first such writer, the IPC socket (Step 9) will be the next.
     */
    signal(SIGPIPE, SIG_IGN);

    /*
     * Bring the logger up first, before anything that can fail.  Until the
     * config is parsed (Step 3) the sink set is stderr at info level; that
     * is then reconfigured from [daemon] log_level / log_sinks / log_file.
     */
    log_init(LOG_LEVEL_INFO);
    (void)log_add_sink(LOG_SINK_STDERR, NULL);

    /* --- Parse command line --- */
    if (parse_args(&ctx, argc, argv) < 0) {
        goto cleanup;   /* cleanup is a no-op this early, but it does run
                           log_shutdown() — every exit path goes through it */
    }

    log_info("netmand %s starting (config: %s)", NETMAND_VERSION,
             ctx.conf_path);

    /* --- Daemonize unless -f --- */
    if (!ctx.foreground) {
        if (daemonize(&ctx) < 0)
            goto cleanup;
    }

    /* --- Create epoll instance --- */
    ctx.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx.epoll_fd < 0) {
        log_error("epoll_create1: %s", strerror(errno));
        goto cleanup;
    }

    /* --- Set up signal handling --- */
    if (setup_signals(&ctx) < 0) {
        goto cleanup;
    }

    /* --- Write PID file --- */
    if (write_pid_file(&ctx) < 0) {
        goto cleanup;
    }

    /*
     * Drop to CAP_NET_ADMIN + CAP_NET_RAW (AD-8).  As privileged sockets
     * are added (netlink in Step 5, AF_PACKET in Step 12) they are opened
     * above this line; nothing below it needs full root.
     */
    if (privileges_drop() < 0) {
        goto cleanup;
    }

    /* TODO: Load config (Step 3) — reconfigures the logger's sinks */
    /* TODO: Initialize netlink (Step 5) */
    /* TODO: Initialize shared memory (Step 9) */
    /* TODO: Initialize IPC socket (Step 10) */

    log_info("initialization complete, PID %d", getpid());

    /* Startup succeeded: release the parent with exit status 0, and let go
     * of the launching terminal. */
    daemon_ready(&ctx, EXIT_SUCCESS);

    /* --- Run event loop --- */
    event_loop_run(&ctx);

    rc = EXIT_SUCCESS;

cleanup:
    /* No-op if the success verdict already went out; otherwise this is what
     * makes a failed start a failed start for whoever launched us. */
    daemon_ready(&ctx, EXIT_FAILURE);

    log_info("shutting down");

    /* Close signalfd */
    if (ctx.signal_fd >= 0)
        close(ctx.signal_fd);

    /* Close epoll fd */
    if (ctx.epoll_fd >= 0)
        close(ctx.epoll_fd);

    /* Remove PID file */
    remove_pid_file(&ctx);

    log_info("exited cleanly");
    log_shutdown();

    return rc;
}
