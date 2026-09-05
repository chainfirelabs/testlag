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

/* One shaping rule.
 * Empty ip AND empty port  =>  apply to ALL traffic on the interface.
 * ip only                   =>  match destination IP.
 * port only                 =>  match destination port (IPv4).
 * ip + port                 =>  match destination IP and port. */
typedef struct {
    char   iface[LAG_IFACE_LEN];
    char   ip[LAG_IP_LEN];        /* "" = any */
    char   port[LAG_PORT_LEN];    /* "" = any (IPv4 dport) */
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
    g_strlcpy(r->ip, "", LAG_IP_LEN);
    g_strlcpy(r->port, "", LAG_PORT_LEN);
    g_strlcpy(r->bandwidth, "", LAG_BW_LEN);
    r->latency_ms = 0.0;
    r->jitter_ms  = 0.0;
    r->drop_pct   = 0.0;
    r->active     = FALSE;
}

/* True if the rule applies to all traffic (no ip, no port). */
static inline gboolean lag_rule_is_all(const LagRule *r) {
    return r->ip[0] == '\0' && r->port[0] == '\0';
}

#endif /* LAG_H */
