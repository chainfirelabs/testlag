/* SPDX-License-Identifier: MIT */
#ifndef LAG_H
#define LAG_H

#include <glib.h>

#define LAG_MAX_RULES   64
#define LAG_IFACE_LEN   64
#define LAG_IP_LEN      64
#define LAG_PORT_LEN    16
#define LAG_BW_LEN      32
/* Upper bound for latency/jitter, in ms; matches the GUI spin buttons and is
 * enforced on the CLI and on profile load. */
#define LAG_MAX_MS      60000
/* htb class rate/ceil: high enough that htb never limits bandwidth; the
 * netem "rate" option is the actual bandwidth limiter. */
#define LAG_CLASS_RATE  "100gbit"

typedef enum { LAG_OUTGOING, LAG_INCOMING } LagDirection;
typedef enum { LAG_ANY, LAG_TCP, LAG_UDP, LAG_SCTP, LAG_ICMP, LAG_ICMPV6 } LagProtocol;

static inline const char *lag_direction_name(LagDirection direction) {
    return direction == LAG_OUTGOING ? "outgoing" :
           direction == LAG_INCOMING ? "incoming" : "invalid";
}
static inline const char *lag_protocol_name(LagProtocol protocol) {
    static const char *names[] = {"any", "tcp", "udp", "sctp", "icmp", "icmpv6"};
    return protocol >= LAG_ANY && protocol <= LAG_ICMPV6 ? names[protocol] : "invalid";
}

/* Source/destination always refer to the packet's actual endpoints. */
typedef struct {
    char   iface[LAG_IFACE_LEN];
    LagDirection direction;
    LagProtocol protocol;
    char   src_ip[LAG_IP_LEN];      /* "" = any */
    char   src_port[LAG_PORT_LEN];  /* "" = any (sport) */
    char   dst_ip[LAG_IP_LEN];      /* "" = any */
    char   dst_port[LAG_PORT_LEN];  /* "" = any (dport) */
    char   bandwidth[LAG_BW_LEN]; /* "" = unlimited, else netem rate e.g. "100kbit" */
    double latency_ms;
    double jitter_ms;
    double drop_pct;
    gboolean active;              /* currently applied via tc */
} LagRule;

typedef struct {
    LagRule rules[LAG_MAX_RULES];
    int     count;
} LagProfile;

static inline void lag_rule_reset(LagRule *r) {
    g_strlcpy(r->iface, "", LAG_IFACE_LEN);
    g_strlcpy(r->src_ip, "", LAG_IP_LEN);
    g_strlcpy(r->src_port, "", LAG_PORT_LEN);
    g_strlcpy(r->dst_ip, "", LAG_IP_LEN);
    g_strlcpy(r->dst_port, "", LAG_PORT_LEN);
    g_strlcpy(r->bandwidth, "", LAG_BW_LEN);
    r->latency_ms = 0.0;
    r->jitter_ms  = 0.0;
    r->drop_pct   = 0.0;
    r->active     = FALSE;
    r->direction  = LAG_OUTGOING;
    r->protocol   = LAG_ANY;
}

/* True if the rule applies to all traffic (no source or destination match). */
static inline gboolean lag_rule_is_all(const LagRule *r) {
    return r->protocol == LAG_ANY && r->src_ip[0] == '\0' && r->src_port[0] == '\0' &&
           r->dst_ip[0] == '\0' && r->dst_port[0] == '\0';
}

#endif /* LAG_H */
