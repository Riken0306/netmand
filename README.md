# netmand

A minimal, purpose-built network management daemon for embedded Linux systems. Designed for Buildroot and Yocto environments with custom init, no systemd dependency, and a small memory footprint. Replaces `udhcpc` + manual `ip` commands with a single, event-driven daemon that applications can query and subscribe to.

---

## Goals

- Manage IPv4 and IPv6 network configuration on embedded Linux targets
- Built-in DHCPv4, DHCPv6, and SLAAC — no external DHCP client dependency
- Expose network state to applications via two channels: a Unix domain socket for commands and events, and a shared memory region for zero-copy status reads
- Config-driven via a simple INI file; reloadable at runtime via `SIGHUP`
- Written in C99, no external library dependencies, single `Makefile` build

---

## Target Platform

- **OS**: Embedded Linux (Buildroot, Yocto)
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
│  JSON · req/resp · events │  │  seqlock · lock-free reads   │
└────────────────┬──────────┘  └───────────┬─────────────────┘
                 └──────────────┬───────────┘
                                │
┌───────────────────────────────▼─────────────────────────────┐
│                    Daemon core (netmand)                     │
│  epoll loop · interface manager · SIGHUP reload · INI config │
└───┬──────────┬───────────┬──────────┬──────────┬────────────┘
    │          │           │          │          │
┌───▼───┐  ┌───▼───┐  ┌───▼────┐  ┌──▼───┐  ┌───▼──────┐
│Netlink│  │ IPv4  │  │  IPv6  │  │ DHCP │  │  Logger  │
│ 2 fds │  │ addrs │  │ addrs  │  │client│  │ ring buf │
└───┬───┘  └───────┘  └────────┘  └──────┘  └──────────┘
    │          │           │          │
┌───▼──────────▼───────────▼──────────▼──────────────────────┐
│             Linux kernel — NETLINK_ROUTE                    │
│   RTM_NEWADDR · RTM_NEWROUTE · RTM_NEWLINK · ICMPv6 RA     │
└─────────────────────────────────────────────────────────────┘
```

All kernel interaction goes through Netlink sockets — no shelling out to `ip` or `ifconfig`. The daemon is strictly single-threaded around an `epoll` loop and takes no locks anywhere: the shared memory region is published under a **seqlock**, which is correct precisely because there is only ever one writer.

Two Netlink sockets are used, not one — an event socket subscribed to the link and address multicast groups, and a separate request socket for dumps and set operations, so that a `RTM_GETLINK` dump can never interleave with and swallow live events.

netmand treats the kernel as the source of truth. Changes go out over Netlink, but internal state only advances when the matching `RTM_NEWADDR`/`RTM_DELADDR` notification comes back. That makes the daemon self-healing when an operator changes something by hand.

A single **interface manager** owns per-interface state: link status, the address table tagged by source (static, DHCPv4, SLAAC, DHCPv6, link-local, foreign), and arbitration between them. Every address module reports into it; it is the only writer of shared memory and the only publisher of events.

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
- DHCPv6 client — Solicit → Advertise → Request → Reply, with a persisted DUID-LL
- SLAAC — **the kernel does it; netmand configures and observes it**
  - netmand sets `net.ipv6.conf.<if>.accept_ra` and `addr_gen_mode` per the interface's configured method
  - The kernel performs Router Solicitation, RA processing, address generation, DAD, prefix lifetime expiry, and the RA default route
  - netmand learns the resulting addresses from `RTM_NEWADDR` and reports them tagged `slaac`, including tentative and DAD-failed states
  - Reimplementing this in userspace would race the kernel unless `accept_ra` were disabled, and would still have to reconstruct DAD and RFC 7217 stable-privacy addressing by hand

### DNS
- DNS servers and search domains from DHCPv4 (options 6 and 15) and DHCPv6 (RDNSS, DNSSL) are merged with any static config and written to `/etc/resolv.conf`
- Written atomically (temp file, `fsync`, `rename`) — a truncated `resolv.conf` breaks every lookup on the device
- `manage_resolv = no` disables it for systems running an external resolver manager

### Hooks
- Optional `up_script` per interface, forked on transition to ready and to down, with `IFACE`, `ACTION`, `IPV4`, `IPV6`, `GATEWAY`, and `DNS` in the environment
- Run with a timeout and reaped via `SIGCHLD` through the signalfd — a hanging hook never stalls the event loop

### IPC — Unix domain socket
Applications connect to `/var/run/netmand.sock` and exchange JSON messages.

The socket is created mode `0660` owned by group `netmand`. Read-only commands are open to that group; commands that change configuration (`set_static`, `trigger_dhcp`) additionally require root or the configured admin gid, checked via `SO_PEERCRED`. Request lines are capped and the client count is bounded.

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
A read-only `struct netmand_state` region is available at a well-known shm path (`/netmand_state`). Applications `mmap()` it `PROT_READ` and read it with no syscalls at all.

Torn reads are prevented by a **seqlock**, not a lock: a `uint32_t seq` is incremented to an odd value before a write and to an even value after, and readers retry while it is odd or changes underneath them. `shm.h` ships the reader loop as an inline helper.

This replaces the `pthread_rwlock_t` earlier drafts placed in the region. An rwlock there deadlocks *every reader, permanently*, if the daemon dies holding the write lock — and unlike mutexes, rwlocks have no `PTHREAD_MUTEX_ROBUST` equivalent. It also embeds libc ABI in a struct shared between separately-compiled processes, which is a real hazard on Buildroot where glibc, musl, and uClibc all appear. The seqlock is crash-safe, carries no libc ABI, and lets netmand link without `-lpthread`.

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
│   │   ├── config.h
│   │   ├── sysctl.c         # per-interface IPv6 sysctls (accept_ra, addr_gen_mode)
│   │   └── hook.c           # fork/exec up-down scripts, SIGCHLD reaping
│   │
│   ├── iface/
│   │   ├── iface.c          # per-interface state machine, address table, reconciliation
│   │   └── iface.h          # THE HUB — every address source reports in here
│   │
│   ├── netlink/
│   │   ├── netlink.c        # event + request sockets, seq tracking, tx queue
│   │   ├── nl_parse.c       # I/O-free message + attribute parsing
│   │   └── netlink.h
│   │
│   ├── ipv4/
│   │   ├── ipv4.c           # RTM_NEWADDR, RTM_NEWROUTE, static assignment
│   │   └── ipv4.h
│   │
│   ├── ipv6/
│   │   ├── ipv6.c           # static IPv6 (SLAAC is the kernel's job — see Features)
│   │   └── ipv6.h
│   │
│   ├── dhcp/
│   │   ├── dhcpv4.c         # DHCPv4 state machine incl. INIT-REBOOT, lease timers
│   │   ├── dhcpv4_msg.c     # I/O-free packet build + option parse
│   │   ├── dhcpv4.h
│   │   ├── dhcpv6.c         # DHCPv6 state machine
│   │   ├── dhcpv6_msg.c     # I/O-free packet build + option parse
│   │   ├── dhcpv6.h
│   │   └── lease.c          # atomic, rate-limited lease persistence
│   │
│   ├── resolv/
│   │   ├── resolv.c         # atomic /etc/resolv.conf writer
│   │   └── resolv.h
│   │
│   ├── ipc/
│   │   ├── ipc.c            # Unix socket server, dispatch, subscriber fan-out
│   │   ├── json.c           # strict flat JSON parser + emitter, no malloc
│   │   └── ipc.h
│   │
│   ├── shm/
│   │   ├── shm.c            # shm_open, mmap, seqlock publish
│   │   └── shm.h            # struct netmand_state layout + inline reader (shared with apps)
│   │
│   ├── timer/
│   │   ├── timer.c          # ONE timerfd + sorted deadline list, T1/T2/backoff
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
├── docs/
│   └── implementation_plan.md   # step-by-step build plan + architecture decisions
│
└── tests/
    ├── test_config.c
    ├── test_iface.c
    ├── test_dhcpv4.c
    ├── test_json.c
    ├── test_logger.c
    ├── test_nl_parse.c
    ├── test_shm.c
    ├── fuzz/                    # one target per wire-format parser
    ├── data/                    # configs + captured packets for namespace tests
    └── Makefile
```

There is no `src/event/` — the event bus is the subscription half of the IPC server and lives in `src/ipc/`. There is no `src/slaac/` — see the IPv6 section.

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
manage_resolv = yes       ; write /etc/resolv.conf from DHCP/static DNS

[eth0]
method = dhcp             ; static | dhcp
; if method = static:
; address = 192.168.1.10
; prefix  = 24
; gateway = 192.168.1.1

[eth0.ipv6]
method = slaac            ; static | dhcpv6 | slaac | off
                          ; slaac = enable kernel accept_ra and observe the result
; if method = static:
; address = 2001:db8::1
; prefix  = 64

; optional: run a script when the interface comes up or goes down
; up_script = /etc/netmand/if-up.sh

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
#define NETMAND_SHM_VERSION       2
#define NETMAND_MAX_IFACES        8
#define NETMAND_MAX_ADDRS_PER_IF  8

typedef struct {
    uint8_t  family;          /* AF_INET | AF_INET6                 */
    uint8_t  prefix;
    uint8_t  source;          /* static | dhcp4 | slaac | dhcp6 | … */
    uint8_t  _pad;
    uint8_t  addr[16];        /* binary; v4 in the first 4 bytes    */
    uint64_t valid_until;     /* CLOCK_MONOTONIC ms, 0 = permanent  */
} netmand_addr_t;

typedef struct {
    char           name[16];
    uint8_t        link_up;
    uint8_t        state;
    uint8_t        addr_count;
    uint8_t        _pad;
    uint8_t        hwaddr[6];
    uint8_t        _pad2[2];
    uint32_t       gw4;           /* network byte order                */
    uint64_t       lease_expires; /* CLOCK_MONOTONIC ms, 0 if not DHCP */
    netmand_addr_t addrs[NETMAND_MAX_ADDRS_PER_IF];
} netmand_iface_state_t;

typedef struct {
    uint32_t version;         /* NETMAND_SHM_VERSION                    */
    uint32_t struct_size;     /* sizeof(netmand_state_t) — client check */
    uint32_t seq;             /* odd = write in progress                */
    uint32_t iface_count;
    netmand_iface_state_t ifaces[NETMAND_MAX_IFACES];
} netmand_state_t;
```

Addresses are stored **binary, and there are several per interface** — link-local, a SLAAC global, and a DHCPv6 address routinely coexist, so a single text-form `ip6_addr` cannot represent reality. Text formatting belongs in the JSON and CLI layers.

Clients must check both `version` and `struct_size` before trusting the region, and should treat a daemon that exited uncleanly as stale — the region survives an unclean shutdown with its last published contents.

---

## Build

```sh
make            # build daemon and netmandctl
make tests      # build and run unit tests
make check      # gcc + clang + ASan/UBSan + namespace tests + cross-build
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

Detailed steps, testing strategy, and the architecture decisions behind them are in
[docs/implementation_plan.md](docs/implementation_plan.md).

### Phase 1 — MVP
- [x] Architecture and folder structure
- [x] Makefile, skeleton `main.c`, signal handlers
- [ ] Fix Step 1 defects, CI, capability dropping *(plan Step 1.5)*
- [ ] Logger (ring buffer + syslog + stderr + file sinks)
- [ ] INI config parser
- [ ] Netlink core (event + request sockets, ack/seq tracking)
- [ ] **Interface manager** — per-interface state machine and address arbitration
- [ ] IPv4 static assignment
- [ ] Shared memory region + `struct netmand_state` (seqlock)
- [ ] Unix socket IPC + JSON dispatcher + event bus
- [ ] `netmandctl` CLI tool
- [ ] Timer subsystem (one timerfd + deadline list)
- [ ] DHCPv4 client, incl. INIT-REBOOT and lease persistence

### Phase 2 — IPv6 + DNS
- [ ] `/etc/resolv.conf` writer + up/down script hooks
- [ ] IPv6 static assignment
- [ ] Kernel SLAAC configuration + observation
- [ ] DHCPv6 client
- [ ] Callback log sink API

### Phase 3 — Hardening
- [ ] Fuzz corpora for every wire-format parser, 24 h per target
- [ ] 24 h stress: concurrent IPC clients, link flapping, lease churn
- [ ] Footprint budget enforced in CI
- [ ] Cross-compile validation on ARMv7 Buildroot, glibc **and musl**
- [ ] 72 h soak on target hardware
- [ ] VLAN sub-interface creation (RTM_NEWLINK + IFLA_LINKINFO) *(unscheduled — see plan Q5)*
- [ ] MAC address cloning *(unscheduled — see plan Q5)*

---

## License

MIT
