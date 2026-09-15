/*
 * netmand — priv.h
 *
 * Privilege model (AD-8): netmand starts as root because it needs
 * netlink and AF_PACKET, but it parses attacker-controlled input
 * (DHCP options, DHCPv6 replies) and must not hold full root while
 * doing so.
 */

#ifndef NETMAND_PRIV_H
#define NETMAND_PRIV_H

/*
 * Drop every capability except CAP_NET_ADMIN and CAP_NET_RAW, clear the
 * inheritable and bounding sets, and set PR_SET_NO_NEW_PRIVS.
 *
 * Call this once, after all privileged sockets are open.  Only the
 * capabilities we already hold are retained, so running unprivileged
 * (unit tests, a developer shell) degrades to "drop everything" rather
 * than failing.
 *
 * Returns 0 on success, -1 on error.
 */
int privileges_drop(void);

#endif /* NETMAND_PRIV_H */
