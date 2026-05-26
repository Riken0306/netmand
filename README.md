# netmand

A minimal, purpose-built network management daemon for embedded Linux systems. Designed for Buildroot environments with custom init, no systemd dependency, and a small memory footprint. Replaces `udhcpc` + manual `ip` commands with a single, event-driven daemon that applications can query and subscribe to.

---

## Goals

- Manage IPv4 and IPv6 network configuration on embedded Linux targets
- Built-in DHCPv4, DHCPv6, and SLAAC — no external DHCP client dependency
- Expose network state to applications via two channels: a Unix domain socket for commands and events, and a shared memory region for zero-copy status reads
- Config-driven via a simple INI file; reloadable at runtime via `SIGHUP`
- Written in C99, no external library dependencies, single `Makefile` build

---

## Target Platform

- **OS**: Embedded Linux (Buildroot)
- **Init system**: Custom init (no systemd)
- **Toolchain**: Cross-compile or native, C99-compatible gcc/clang
- **Kernel requirement**: Linux 2.6.32+ (netlink, timerfd, epoll, shm)

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        Applications                         │
│              (VoIP stack, Edge AI, custom daemons)          │
└────────────────┬──────────────────────────┬─────────────────┘
                 │ Unix socket              │ mmap (read-only)
                 │ commands + async events  │ status struct
┌────────────────▼──────────┐  ┌───────────▼─────────────────┐
│     Unix socket API       │  │     Shared memory region     │
│  JSON · req/resp · events │  │  pthread_rwlock · state buf  │
└────────────────┬──────────┘  └───────────┬─────────────────┘
                 └──────────────┬───────────┘
                                │
┌───────────────────────────────▼─────────────────────────────┐
│                    Daemon core (netmand)                     │
│        epoll event loop · SIGHUP reload · INI config        │
└───┬──────────┬───────────┬──────────┬──────────┬────────────┘
    │          │           │          │          │
┌───▼──┐  ┌───▼───┐  ┌────▼───┐  ┌───▼──┐  ┌───▼──────┐
│Netlink│  │ IPv4  │  │  IPv6  │  │ DHCP │  │  Logger  │
│      │  │manager│  │manager │  │client│  │ring buf  │
└───┬──┘  └───────┘  └────────┘  └──────┘  └──────────┘
    │          │           │          │
┌───▼──────────▼───────────▼──────────▼──────────────────────┐
│             Linux kernel — NETLINK_ROUTE                    │
│   RTM_NEWADDR · RTM_NEWROUTE · RTM_NEWLINK · ICMPv6 RA     │
└─────────────────────────────────────────────────────────────┘
```

All kernel interaction goes through Netlink sockets — no shelling out to `ip` or `ifconfig`. The daemon is single-threaded around an `epoll` loop; the only thread boundary is the `pthread_rwlock` guarding the shared memory region.

---

## Features

### IPv4
- Static address assignment (addr + prefix + gateway)
- DHCPv4 client — built-in raw socket implementation
  - DISCOVER → OFFER → REQUEST → ACK state machine
  - Lease renewal via T1/T2 timers
  - Rebind and lease expiry handling

### IPv6
- Static address assignment
- DHCPv6 client — Solicit → Advertise → Request → Reply
- SLAAC — ICMPv6 Router Advertisement listener
  - EUI-64 interface identifier construction
  - Prefix lifetime and router lifetime tracking

### IPC — Unix domain socket
Applications connect to `/var/run/netmand.sock` and exchange JSON messages.

Supported commands:

| Command | Description |
|---|---|
| `get_state` | Returns full interface state (IPs, link, gateway, lease expiry) |
| `set_static` | Assign a static IPv4 or IPv6 address |
| `trigger_dhcp` | Force a DHCP renew/rebind on an interface |
| `subscribe` | Register for async event notifications |
| `list_ifaces` | List all managed interfaces |

Async events pushed to subscribed sockets:

| Event | Trigger |
|---|---|
| `link_up` | Interface carrier detected |
| `link_down` | Interface carrier lost |
| `ip_acquired` | Address assigned (DHCP or SLAAC) |
| `ip_lost` | Address removed or lease expired |
| `dhcp_renew` | Lease successfully renewed |

### IPC — Shared memory
A read-only `struct netmand_state` region is available at a well-known shm path (`/netmand_state`). Applications `mmap()` it directly for zero-syscall status reads. A `pthread_rwlock_t` at the head of the struct protects against torn reads during daemon writes.

Suitable for tight-loop polling (VoIP link-state checks, watchdog threads). For event-driven use, the Unix socket is simpler.

### Logging
A multi-sink ring buffer. Log entries are written to the ring first, then flushed to any combination of:

- **syslog** — standard embedded log aggregation
- **stderr** — useful during development and init startup
- **file** — persistent ring buffer at `/var/log/netmand.log`
- **callback** — `void (*log_cb)(int level, const char *msg)` registered by the application for in-process log capture

Log level, ring buffer size, and active sinks are all configurable at runtime.

---

## Repository Layout

```
netmand/
├── Makefile
├── README.md
│
├── src/
│   ├── core/
│   │   ├── main.c           # daemon entry point, signal handlers, epoll loop
│   │   ├── main.h
│   │   ├── config.c         # INI parser, struct iface_config, struct daemon_config
│   │   └── config.h
│   │
│   ├── netlink/
│   │   ├── netlink.c        # NETLINK_ROUTE socket, nl_send/nl_recv helpers
│   │   └── netlink.h
│   │
│   ├── ipv4/
│   │   ├── ipv4.c           # RTM_NEWADDR, RTM_NEWROUTE, static assignment
│   │   └── ipv4.h
│   │
│   ├── ipv6/
│   │   ├── ipv6.c           # static IPv6, EUI-64 construction
│   │   └── ipv6.h
│   │
│   ├── dhcp/
│   │   ├── dhcpv4.c         # raw socket DHCPv4 state machine, lease timers
│   │   ├── dhcpv4.h
│   │   ├── dhcpv6.c         # DHCPv6 state machine
│   │   └── dhcpv6.h
│   │
│   ├── slaac/
│   │   ├── slaac.c          # ICMPv6 RA listener, prefix/router lifetime
│   │   └── slaac.h
│   │
│   ├── ipc/
│   │   ├── ipc.c            # Unix socket server, JSON command dispatcher
│   │   └── ipc.h
│   │
│   ├── shm/
│   │   ├── shm.c            # shm_open, mmap, rwlock, state writes
│   │   └── shm.h            # struct netmand_state layout (shared with apps)
│   │
│   ├── event/
│   │   ├── event.c          # event bus — subscribe, publish, notify
│   │   └── event.h
│   │
│   ├── timer/
│   │   ├── timer.c          # timerfd-based wheel, T1/T2/RA lifetime callbacks
│   │   └── timer.h
│   │
│   └── logger/
│       ├── logger.c         # ring buffer, syslog/stderr/file/callback sinks
│       └── logger.h
│
├── tools/
│   ├── netmandctl.c         # CLI client — send JSON commands, print responses
│   └── Makefile
│
├── conf/
│   └── netmand.conf         # example INI config (see Configuration section)
│
└── tests/
    ├── test_config.c
    ├── test_dhcpv4.c
    ├── test_logger.c
    └── Makefile
```

---

## Configuration

`/etc/netmand/netmand.conf` — INI format, reloaded on `SIGHUP`.

```ini
[daemon]
log_level = info          ; debug | info | warn | error
log_sinks = syslog,file   ; syslog | stderr | file | callback (comma-separated)
log_file  = /var/log/netmand.log
shm_path  = /netmand_state
socket_path = /var/run/netmand.sock

[eth0]
method = dhcp             ; static | dhcp
; if method = static:
; address = 192.168.1.10
; prefix  = 24
; gateway = 192.168.1.1

[eth0.ipv6]
method = slaac            ; static | dhcpv6 | slaac | off
; if method = static:
; address = 2001:db8::1
; prefix  = 64

[eth1]
method = static
address = 10.0.0.1
prefix  = 24
```

---

## IPC Protocol

Commands and responses over `/var/run/netmand.sock` are newline-terminated JSON objects.

**Request:**
```json
{ "cmd": "get_state", "iface": "eth0" }
```

**Response:**
```json
{
  "iface": "eth0",
  "link":  "up",
  "ip4":   "192.168.1.10/24",
  "gw4":   "192.168.1.1",
  "ip6":   "2001:db8::a1b2/64",
  "lease_expires": 1748900000
}
```

**Async event (pushed to subscribed sockets):**
```json
{ "event": "ip_acquired", "iface": "eth0", "ip4": "192.168.1.10/24" }
```

---

## Shared Memory Layout

`shm.h` defines `struct netmand_state`, the exact layout of the mmap region. Applications include this header and call `shm_open` + `mmap` to get a read pointer. The `uint32_t version` field at offset 0 allows detecting ABI changes without recompilation.

```c
/* shm.h — included by both daemon and client applications */
#define NETMAND_SHM_VERSION  1
#define NETMAND_MAX_IFACES   8

typedef struct {
    char     name[16];
    uint8_t  link_up;
    uint32_t ip4_addr;      /* host byte order */
    uint8_t  ip4_prefix;
    uint32_t ip4_gateway;
    char     ip6_addr[40];  /* text form */
    uint8_t  ip6_prefix;
    uint64_t lease_expires; /* unix timestamp, 0 if static */
} netmand_iface_state_t;

typedef struct {
    uint32_t              version;
    pthread_rwlock_t      lock;
    uint8_t               iface_count;
    netmand_iface_state_t ifaces[NETMAND_MAX_IFACES];
} netmand_state_t;
```

---

## Build

```sh
make            # build daemon and netmandctl
make tests      # build and run unit tests
make clean

# cross-compile example (Buildroot toolchain)
make CC=arm-linux-gnueabihf-gcc
```

Output: `build/netmand`, `build/netmandctl`

---

## Init Integration (Buildroot custom init)

Add an init script at `/etc/init.d/S40netmand`:

```sh
#!/bin/sh
case "$1" in
  start) /usr/sbin/netmand -c /etc/netmand/netmand.conf & ;;
  stop)  kill $(cat /var/run/netmand.pid) ;;
  reload) kill -HUP $(cat /var/run/netmand.pid) ;;
esac
```

---

## Development Roadmap

### Phase 1 — MVP
- [x] Architecture and folder structure
- [ ] Makefile, skeleton `main.c`, signal handlers
- [ ] INI config parser
- [ ] Logger (ring buffer + syslog + stderr + file sinks)
- [ ] Netlink core (bring up/down, RTM helpers)
- [ ] IPv4 static assignment
- [ ] Shared memory region + `struct netmand_state`
- [ ] Unix socket IPC + JSON dispatcher
- [ ] `netmandctl` CLI tool
- [ ] DHCPv4 client (raw socket, full state machine)
- [ ] Timer wheel (timerfd + epoll)

### Phase 2 — IPv6 + events
- [ ] IPv6 static assignment
- [ ] SLAAC (ICMPv6 RA listener, EUI-64)
- [ ] DHCPv6 client
- [ ] Event bus + async socket notifications
- [ ] Callback log sink API

### Phase 3 — Hardening
- [ ] VLAN sub-interface creation (RTM_NEWLINK + IFLA_LINKINFO)
- [ ] MAC address cloning
- [ ] Lease persistence across restarts
- [ ] Unit tests for DHCPv4 state machine and config parser
- [ ] Cross-compile validation on ARMv7 Buildroot target

---

## License

MIT
