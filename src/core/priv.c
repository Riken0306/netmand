/*
 * netmand — priv.c
 *
 * Capability dropping via the raw capget/capset syscalls.  libcap is not
 * linked: two syscalls and a prctl loop do not justify a dependency on a
 * Buildroot target.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* for syscall() */
#endif

#include "priv.h"

#include <errno.h>
#include <linux/capability.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Capabilities netmand keeps for its whole life:
 *   CAP_NET_ADMIN — netlink address / route / link changes, IPv6 sysctls
 *   CAP_NET_RAW   — AF_PACKET socket for the DHCPv4 client
 * Both are below 32, so they live entirely in capability word 0. */
#define KEEP_MASK  ((uint32_t)((1U << CAP_NET_ADMIN) | (1U << CAP_NET_RAW)))

static int cap_keep(int cap)
{
    return cap == CAP_NET_ADMIN || cap == CAP_NET_RAW;
}

int privileges_drop(void)
{
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct   data[2];
    uint32_t keep;
    int cap;

    memset(&hdr, 0, sizeof(hdr));
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    hdr.pid     = 0;   /* self */

    memset(data, 0, sizeof(data));
    if (syscall(SYS_capget, &hdr, data) < 0) {
        fprintf(stderr, "netmand: capget: %s\n", strerror(errno));
        return -1;
    }
    keep = data[0].permitted & KEEP_MASK;

    /*
     * Bounding set first: shrinking it needs CAP_SETPCAP, which the
     * capset below takes away.  An empty-but-for-net bounding set means
     * no exec of a setuid binary can hand a capability back.
     */
    for (cap = 0; cap <= CAP_LAST_CAP; cap++) {
        if (cap_keep(cap))
            continue;
        if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0 && errno != EINVAL) {
            /* EPERM here just means we never had CAP_SETPCAP (e.g. running
             * as an ordinary user); the capset below still applies. */
            if (errno != EPERM) {
                fprintf(stderr, "netmand: PR_CAPBSET_DROP %d: %s\n",
                        cap, strerror(errno));
                return -1;
            }
            break;
        }
    }

    /* Effective = permitted = the two we keep; inheritable stays empty so
     * up/down hook scripts inherit nothing. */
    memset(data, 0, sizeof(data));
    data[0].effective = keep;
    data[0].permitted = keep;
    if (syscall(SYS_capset, &hdr, data) < 0) {
        fprintf(stderr, "netmand: capset: %s\n", strerror(errno));
        return -1;
    }

    /* No exec can ever raise privileges from here. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        fprintf(stderr, "netmand: PR_SET_NO_NEW_PRIVS: %s\n", strerror(errno));
        return -1;
    }

    if (keep == KEEP_MASK)
        fprintf(stderr, "netmand: capabilities reduced to "
                "cap_net_admin,cap_net_raw\n");
    else
        fprintf(stderr, "netmand: not started privileged; capabilities "
                "dropped to 0x%08x (network changes will fail)\n",
                (unsigned)keep);
    return 0;
}
