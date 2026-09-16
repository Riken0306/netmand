/*
 * netmand — main.h
 *
 * Daemon context structure and core declarations.
 * A single struct netmand_ctx is allocated in main() and passed by pointer
 * to every subsystem — no global state.
 */

#ifndef NETMAND_MAIN_H
#define NETMAND_MAIN_H

#include <stdbool.h>
#include <stdint.h>

/* --------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------- */

#define NETMAND_VERSION        "0.1.0"
#define NETMAND_DEFAULT_CONF   "/etc/netmand/netmand.conf"
#define NETMAND_PID_FILE       "/var/run/netmand.pid"
#define NETMAND_MAX_EPOLL_EVS  64

/* --------------------------------------------------------------------
 * Epoll handler — generic fd callback registered with the event loop
 * ----------------------------------------------------------------- */

struct netmand_ctx;  /* forward declaration */

typedef struct epoll_handler {
    int  fd;
    void (*callback)(struct netmand_ctx *ctx, int fd, uint32_t events);
} epoll_handler_t;

/* --------------------------------------------------------------------
 * Daemon context — the root of all daemon state
 * ----------------------------------------------------------------- */

typedef struct netmand_ctx {
    /* Configuration */
    const char *conf_path;       /* path to INI config file            */
    bool        foreground;      /* -f flag: stay in foreground         */
    bool        keep_stdio;      /* -s flag: keep stdout/stderr after
                                    startup instead of /dev/null        */

    /* Event loop */
    int         epoll_fd;        /* epoll file descriptor               */
    bool        running;         /* set to false to break the loop      */
    bool        reload_pending;  /* set by SIGHUP handler               */

    /* Signal handling */
    int         signal_fd;       /* signalfd for SIGTERM/SIGINT/SIGHUP  */

    /* Daemonize readiness handshake.  Write end of the pipe the forked
     * child uses to tell the parent whether startup succeeded; -1 in the
     * foreground, and again once the verdict has been sent. */
    int         ready_fd;

    /* PID file */
    const char *pid_path;        /* PID file path                       */
    bool        pid_file_owned;  /* true once *we* wrote the PID file    */

    /* Handlers owned by core.  Modules keep their own handler structs in
     * their own context; nothing in netmand lives at file scope. */
    epoll_handler_t signal_handler;
} netmand_ctx_t;

/* --------------------------------------------------------------------
 * Event loop helpers
 * ----------------------------------------------------------------- */

/*
 * Register a handler with the epoll loop; handler->fd is the fd to watch.
 * The handler struct must remain valid for the lifetime of the registration.
 * Returns 0 on success, -1 on error.
 */
int  event_loop_add(netmand_ctx_t *ctx, epoll_handler_t *handler,
                    uint32_t events);

/*
 * Remove an fd from the epoll loop.
 * Returns 0 on success, -1 on error.
 */
int  event_loop_remove(netmand_ctx_t *ctx, int fd);

/*
 * Run the event loop until ctx->running becomes false.
 */
void event_loop_run(netmand_ctx_t *ctx);

#endif /* NETMAND_MAIN_H */
