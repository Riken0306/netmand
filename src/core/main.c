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
static int  write_pid_file(const netmand_ctx_t *ctx);
static void remove_pid_file(const netmand_ctx_t *ctx);
static int  setup_signals(netmand_ctx_t *ctx);
static void handle_signal(netmand_ctx_t *ctx, int fd, uint32_t events);
static int  daemonize(void);

/* ====================================================================
 * Event loop implementation
 * ================================================================= */

int event_loop_add(netmand_ctx_t *ctx, int fd, uint32_t events,
                   epoll_handler_t *handler)
{
    struct epoll_event ev = {
        .events = events,
        .data.ptr = handler,
    };
    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        fprintf(stderr, "epoll_ctl ADD fd=%d: %s\n", fd, strerror(errno));
        return -1;
    }
    return 0;
}

int event_loop_remove(netmand_ctx_t *ctx, int fd)
{
    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        fprintf(stderr, "epoll_ctl DEL fd=%d: %s\n", fd, strerror(errno));
        return -1;
    }
    return 0;
}

void event_loop_run(netmand_ctx_t *ctx)
{
    struct epoll_event events[NETMAND_MAX_EPOLL_EVS];

    fprintf(stderr, "netmand: entering event loop\n");

    while (ctx->running) {
        int nfds = epoll_wait(ctx->epoll_fd, events, NETMAND_MAX_EPOLL_EVS, -1);
        if (nfds < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "epoll_wait: %s\n", strerror(errno));
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
            fprintf(stderr, "netmand: config reload requested "
                    "(stub — config parser not yet implemented)\n");
            /* TODO: call config_reload(ctx) once config.c exists */
        }
    }

    fprintf(stderr, "netmand: event loop exited\n");
}

/* ====================================================================
 * Signal handling via signalfd
 * ================================================================= */

static epoll_handler_t signal_handler_entry;

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
        fprintf(stderr, "sigprocmask: %s\n", strerror(errno));
        return -1;
    }

    ctx->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (ctx->signal_fd < 0) {
        fprintf(stderr, "signalfd: %s\n", strerror(errno));
        return -1;
    }

    /* Register the signalfd with the epoll loop. */
    signal_handler_entry.fd = ctx->signal_fd;
    signal_handler_entry.callback = handle_signal;

    if (event_loop_add(ctx, ctx->signal_fd, EPOLLIN, &signal_handler_entry) < 0) {
        close(ctx->signal_fd);
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
            fprintf(stderr, "netmand: received signal %u, shutting down\n",
                    si.ssi_signo);
            ctx->running = false;
            break;

        case SIGHUP:
            fprintf(stderr, "netmand: received SIGHUP, scheduling reload\n");
            ctx->reload_pending = true;
            break;

        default:
            fprintf(stderr, "netmand: unexpected signal %u\n", si.ssi_signo);
            break;
        }
    }
}

/* ====================================================================
 * PID file management
 * ================================================================= */

static int write_pid_file(const netmand_ctx_t *ctx)
{
    /* Check for a stale PID file. */
    FILE *f = fopen(ctx->pid_path, "r");
    if (f) {
        pid_t old_pid = 0;
        if (fscanf(f, "%d", &old_pid) == 1 && old_pid > 0) {
            /* Check if process is still running. */
            if (kill(old_pid, 0) == 0) {
                fprintf(stderr, "netmand: already running (pid %d)\n",
                        old_pid);
                fclose(f);
                return -1;
            }
            /* Stale PID file, safe to overwrite. */
            fprintf(stderr, "netmand: removing stale PID file (pid %d)\n",
                    old_pid);
        }
        fclose(f);
    }

    f = fopen(ctx->pid_path, "w");
    if (!f) {
        fprintf(stderr, "netmand: cannot write PID file %s: %s\n",
                ctx->pid_path, strerror(errno));
        return -1;
    }
    fprintf(f, "%d\n", getpid());
    fclose(f);

    return 0;
}

static void remove_pid_file(const netmand_ctx_t *ctx)
{
    unlink(ctx->pid_path);
}

/* ====================================================================
 * Daemonize — fork, setsid, redirect stdio to /dev/null
 * ================================================================= */

static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork: %s\n", strerror(errno));
        return -1;
    }
    if (pid > 0) {
        /* Parent exits. */
        _exit(EXIT_SUCCESS);
    }

    /* Child becomes session leader. */
    if (setsid() < 0) {
        fprintf(stderr, "setsid: %s\n", strerror(errno));
        return -1;
    }

    /* Redirect stdin/stdout/stderr to /dev/null. */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
            close(devnull);
    }

    /* Working directory to / so we don't hold any mount busy. */
    if (chdir("/") < 0) {
        /* Non-fatal, but worth noting (can't log to stderr anymore). */
    }

    return 0;
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
    ctx->foreground = true;

    while ((opt = getopt(argc, argv, "c:fp:vh")) != -1) {
        switch (opt) {
        case 'c':
            ctx->conf_path = optarg;
            break;
        case 'f':
            ctx->foreground = true;
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

    /* --- Parse command line --- */
    if (parse_args(&ctx, argc, argv) < 0) {
        return EXIT_FAILURE;
    }

    fprintf(stderr, "netmand %s starting (config: %s)\n",
            NETMAND_VERSION, ctx.conf_path);

    /* --- Daemonize unless -f --- */
    if (!ctx.foreground) {
        if (daemonize() < 0)
            return EXIT_FAILURE;
    }

    /* --- Create epoll instance --- */
    ctx.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx.epoll_fd < 0) {
        fprintf(stderr, "epoll_create1: %s\n", strerror(errno));
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

    /* TODO: Load config (Step 3) */
    /* TODO: Initialize logger (Step 2) */
    /* TODO: Initialize netlink (Step 5) */
    /* TODO: Initialize shared memory (Step 9) */
    /* TODO: Initialize IPC socket (Step 10) */

    fprintf(stderr, "netmand: initialization complete, PID %d\n", getpid());

    /* --- Run event loop --- */
    event_loop_run(&ctx);

    rc = EXIT_SUCCESS;

cleanup:
    fprintf(stderr, "netmand: shutting down\n");

    /* Close signalfd */
    if (ctx.signal_fd >= 0)
        close(ctx.signal_fd);

    /* Close epoll fd */
    if (ctx.epoll_fd >= 0)
        close(ctx.epoll_fd);

    /* Remove PID file */
    remove_pid_file(&ctx);

    fprintf(stderr, "netmand: exited cleanly\n");
    return rc;
}
