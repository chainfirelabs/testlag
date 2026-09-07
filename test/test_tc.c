/* SPDX-License-Identifier: MIT */
/* Headless test for the tc backend + profile round-trip.
 *
 * Run inside a private network namespace with net-admin:
 *   unshare -r -n ./test/test_tc
 *
 * (Inside the namespace we are root, so no sudo path is exercised.)
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lag.h"
#include "profile.h"
#include "tc.h"

static int failures = 0;

#define CHECK(cond, fmt, ...) do { \
    if (cond) { printf("  ok   - " fmt "\n", ##__VA_ARGS__); } \
    else      { printf("  FAIL - " fmt "\n", ##__VA_ARGS__); failures++; } \
} while (0)

int main(void) {
    printf("root=%d sudo_present=%d use_sudo=%d\n",
        tc_is_root(), tc_sudo_present(), tc_get_use_sudo());

    /* ---- interface discovery ---- */
    GPtrArray *ifs = tc_list_interfaces();
    printf("interfaces:");
    for (guint i = 0; i < ifs->len; i++)
        printf(" %s", (const char *)g_ptr_array_index(ifs, i));
    printf("\n");
    CHECK(ifs->len > 0, "found at least one interface");
    g_ptr_array_free(ifs, TRUE);

    /* ---- validators (also the shell-injection guard) ---- */
    CHECK(tc_valid_iface("eth0"), "valid iface eth0");
    CHECK(!tc_valid_iface("eth0; rm -rf /"), "rejects iface injection");
    CHECK(tc_valid_ip("1.2.3.4"), "valid ipv4");
    CHECK(tc_valid_ip("2001:db8::1"), "valid ipv6");
    CHECK(!tc_valid_ip("999.1.1.1"), "rejects bad ipv4");
    CHECK(!tc_valid_ip("1.2.3.4; reboot"), "rejects ip injection");
    CHECK(tc_valid_port("443"), "valid port");
    CHECK(!tc_valid_port("99999"), "rejects bad port");
    CHECK(tc_valid_rate("100kbit"), "valid rate");
    CHECK(!tc_valid_rate("100kbit;reboot"), "rejects rate injection");
    CHECK(!tc_valid_ip("1.2.3.4.5.6.7"), "rejects trailing octets");
    CHECK(!tc_valid_iface("-h"), "rejects iface that looks like an option");
    CHECK(!tc_valid_rate("abc"), "rejects non-numeric rate");
    CHECK(tc_valid_rate("1.5mbit"), "valid fractional rate");
    CHECK(tc_valid_ms(0) && tc_valid_ms(LAG_MAX_MS), "ms range accepts bounds");
    CHECK(!tc_valid_ms(-1) && !tc_valid_ms(LAG_MAX_MS + 1), "ms range rejects out of range");
    CHECK(!tc_valid_ms(1.0 / 0.0) && !tc_valid_ms(0.0 / 0.0), "ms range rejects inf/nan");
    CHECK(tc_valid_pct(100) && !tc_valid_pct(101), "pct range");

    /* a well-formed rule passes, a rule carrying shell metacharacters does not */
    LagRule vr;
    lag_rule_reset(&vr);
    g_strlcpy(vr.iface, "lo", sizeof vr.iface);
    vr.latency_ms = 10;
    char verr[256];
    CHECK(tc_valid_rule(&vr, verr, sizeof verr), "valid rule accepted");
    g_strlcpy(vr.iface, "lo;id", sizeof vr.iface);
    CHECK(!tc_valid_rule(&vr, verr, sizeof verr), "rule with injected iface rejected");

    /* ---- build a profile: specific rule + all-traffic rule on lo ---- */
    LagProfile p;
    profile_clear(&p);
    LagRule *r1 = &p.rules[p.count++];
    g_strlcpy(r1->iface, "lo", sizeof r1->iface);
    g_strlcpy(r1->dst_ip, "127.0.0.1", sizeof r1->dst_ip);
    g_strlcpy(r1->dst_port, "443", sizeof r1->dst_port);
    g_strlcpy(r1->bandwidth, "100kbit", sizeof r1->bandwidth);
    r1->latency_ms = 100; r1->jitter_ms = 20; r1->drop_pct = 5;

    LagRule *r2 = &p.rules[p.count++];
    g_strlcpy(r2->iface, "lo", sizeof r2->iface);
    r2->latency_ms = 300; /* all-traffic rule */

    /* ---- command builders ---- */
    char *netem = tc_build_netem(r1);
    printf("netem r1 : %s\n", netem);
    CHECK(strstr(netem, "delay 100ms 20ms") != NULL, "netem delay+jitter");
    CHECK(strstr(netem, "loss 5%") != NULL, "netem loss");
    CHECK(strstr(netem, "rate 100kbit") != NULL, "netem rate");
    g_free(netem);

    LagRule nojit;
    lag_rule_reset(&nojit);
    nojit.latency_ms = 50; /* jitter 0: must still say "ms", not "m" */
    char *netem2 = tc_build_netem(&nojit);
    printf("netem nojit: %s\n", netem2);
    CHECK(strcmp(netem2, "netem delay 50ms") == 0, "netem no-jitter suffix");
    g_free(netem2);

    char *filt = tc_build_filter(r1, "lo", 2);
    printf("filter r1 : %s\n", filt);
    CHECK(filt != NULL, "specific rule has filter");
    CHECK(filt && strstr(filt, "dst_ip 127.0.0.1/32") != NULL, "filter dst");
    CHECK(filt && strstr(filt, "dst_port 443") != NULL, "filter dport");
    CHECK(filt && strstr(filt, "classid 1:2") != NULL, "filter flowid");
    g_free(filt);

    CHECK(tc_build_filter(r2, "lo", 1) == NULL, "all-traffic has no filter");

    /* ---- source matching ----
     * netem shapes egress, so "lag whoever talks to my web server" is
     * expressed as its replies: source port 80, any destination. */
    LagRule srv;
    lag_rule_reset(&srv);
    g_strlcpy(srv.iface, "lo", sizeof srv.iface);
    g_strlcpy(srv.src_port, "80", sizeof srv.src_port);
    srv.latency_ms = 200;
    CHECK(!lag_rule_is_all(&srv), "a source-only rule is not an all-traffic rule");
    CHECK(tc_valid_rule(&srv, verr, sizeof verr), "source-only rule validates (%s)", verr);
    char *sfilt = tc_build_filter(&srv, "lo", 2);
    printf("filter src-only: %s\n", sfilt);
    CHECK(sfilt && strstr(sfilt, "src_port 80") != NULL, "filter sport");
    CHECK(sfilt && strstr(sfilt, "dport") == NULL, "source-only rule has no dport match");
    g_free(sfilt);

    /* one device talking to that service: its replies, aimed at that device */
    g_strlcpy(srv.dst_ip, "192.168.1.77", sizeof srv.dst_ip);
    sfilt = tc_build_filter(&srv, "lo", 3);
    printf("filter src+dst : %s\n", sfilt);
    CHECK(sfilt && strstr(sfilt, "dst_ip 192.168.1.77/32") != NULL, "filter src+dst: dst");
    CHECK(sfilt && strstr(sfilt, "src_port 80") != NULL, "filter src+dst: sport");
    g_free(sfilt);

    /* full four-tuple, and the v4/v6 mixing guard */
    LagRule four;
    lag_rule_reset(&four);
    g_strlcpy(four.iface, "lo", sizeof four.iface);
    g_strlcpy(four.src_ip, "10.0.0.1", sizeof four.src_ip);
    g_strlcpy(four.src_port, "80", sizeof four.src_port);
    g_strlcpy(four.dst_ip, "10.0.0.2", sizeof four.dst_ip);
    g_strlcpy(four.dst_port, "9000", sizeof four.dst_port);
    four.latency_ms = 10;
    sfilt = tc_build_filter(&four, "lo", 4);
    printf("filter 4-tuple : %s\n", sfilt);
    CHECK(sfilt && strstr(sfilt, "src_ip 10.0.0.1/32") &&
          strstr(sfilt, "dst_ip 10.0.0.2/32") &&
          strstr(sfilt, "src_port 80") &&
          strstr(sfilt, "dst_port 9000"), "filter carries all four matches");
    g_free(sfilt);
    g_strlcpy(four.dst_ip, "2001:db8::1", sizeof four.dst_ip);
    CHECK(!tc_valid_rule(&four, verr, sizeof verr), "rejects a rule mixing IPv4 and IPv6");

    LagRule v6;
    lag_rule_reset(&v6);
    g_strlcpy(v6.iface, "lo", sizeof v6.iface);
    g_strlcpy(v6.src_ip, "2001:db8::1", sizeof v6.src_ip);
    v6.latency_ms = 10;
    sfilt = tc_build_filter(&v6, "lo", 5);
    printf("filter v6 src  : %s\n", sfilt);
    CHECK(sfilt && strstr(sfilt, "protocol ipv6") && strstr(sfilt, "src_ip 2001:db8::1/128"),
          "IPv6 source match");
    g_free(sfilt);

    /* CIDR boundaries, protocol validation and dual-stack filter priorities. */
    CHECK(tc_valid_ip("10.1.2.3/0") && tc_valid_ip("10.1.2.3/32") &&
          tc_valid_ip("2001:db8::1/128"), "accepts CIDR boundaries");
    CHECK(!tc_valid_ip("10.1.2.3/33") && !tc_valid_ip("::1/129") &&
          !tc_valid_ip("::1/-1") && !tc_valid_ip("10.0.0.1/"), "rejects invalid CIDRs");
    LagRule subnet;
    lag_rule_reset(&subnet);
    g_strlcpy(subnet.iface, "lo", sizeof subnet.iface);
    g_strlcpy(subnet.src_ip, "10.1.2.99/24", sizeof subnet.src_ip);
    sfilt = tc_build_filter(&subnet, "lo", 2);
    CHECK(strstr(sfilt, "src_ip 10.1.2.0/24"), "normalizes CIDR host bits");
    g_free(sfilt);
    subnet.src_ip[0] = '\0';
    g_strlcpy(subnet.dst_port, "443", sizeof subnet.dst_port);
    GPtrArray *variants = tc_build_filters(&subnet, "lo", 2);
    CHECK(variants->len == 6, "any port matches three transports in both families");
    CHECK(strstr(g_ptr_array_index(variants, 0), "protocol ip pref 4") &&
          strstr(g_ptr_array_index(variants, 3), "protocol ipv6 pref 5"),
          "IP families use distinct classifier priorities");
    g_ptr_array_free(variants, TRUE);
    subnet.protocol = LAG_ICMP;
    CHECK(!tc_valid_rule(&subnet, verr, sizeof verr), "rejects ICMP ports");
    subnet.dst_port[0] = '\0';
    subnet.protocol = LAG_ICMPV6;
    variants = tc_build_filters(&subnet, "lo", 2);
    CHECK(variants->len == 1 && strstr(g_ptr_array_index(variants, 0), "protocol ipv6"),
          "ICMPv6 selects only IPv6");
    g_ptr_array_free(variants, TRUE);

    /* ---- apply both active ---- */
    char err[2048];
    r1->active = TRUE;
    r2->active = TRUE;
    int rc = tc_apply_interface(&p, "lo", err, sizeof err);
    printf("apply rc=%d%s%s\n", rc, rc ? "\n" : "", rc ? err : "");
    CHECK(rc == 0, "apply both rules");

    char *state = tc_get_state_text("lo");
    printf("STATE:\n%s\n", state);
    CHECK(strstr(state, "htb") != NULL, "state has htb root");
    CHECK(strstr(state, "netem") != NULL, "state has netem");
    CHECK(strstr(state, "100ms") != NULL, "state shows 100ms delay");
    CHECK(strstr(state, "300ms") != NULL, "state shows 300ms all-traffic delay");
    g_free(state);

    /* ---- stop the specific rule; all-traffic must remain ---- */
    r1->active = FALSE;
    rc = tc_apply_interface(&p, "lo", err, sizeof err);
    CHECK(rc == 0, "re-apply after stopping specific rule");
    state = tc_get_state_text("lo");
    printf("STATE(after stop r1):\n%s\n", state);
    CHECK(strstr(state, "100ms") == NULL, "100ms rule gone");
    CHECK(strstr(state, "300ms") != NULL, "all-traffic 300ms still there");
    g_free(state);

    /* ---- stop all -> interface back to clean state ---- */
    r2->active = FALSE;
    rc = tc_apply_interface(&p, "lo", err, sizeof err);
    CHECK(rc == 0, "re-apply with no active rules");
    state = tc_get_state_text("lo");
    printf("STATE(cleared):\n%s\n", state);
    CHECK(strstr(state, "netem") == NULL, "no netem after clear");
    CHECK(strstr(state, "htb") == NULL, "no htb after clear");
    g_free(state);

    /* ---- specific-only path: no all-traffic rule ----
     * class 1:1 (htb default) must get a pass-through qdisc, and
     * unmatched traffic must still flow. */
    if (system("ip link set lo up") != 0)
        printf("  note - could not bring lo up\n");
    r1->active = TRUE; /* specific rule only */
    rc = tc_apply_interface(&p, "lo", err, sizeof err);
    CHECK(rc == 0, "apply specific-only rule (%s)", err);
    state = tc_get_state_text("lo");
    printf("STATE(specific-only):\n%s\n", state);
    CHECK(strstr(state, "htb") != NULL, "specific-only: htb root");
    CHECK(strstr(state, "netem") != NULL, "specific-only: netem on 1:2");
    CHECK(strstr(state, "100ms") != NULL, "specific-only: 100ms delay");
    CHECK(strstr(state, "pfifo") != NULL, "specific-only: pass-through pfifo on 1:1");
    g_free(state);
    /* ping a destination that matches no filter: must pass through 1:1 */
    int prc = system("ping -c 2 -W 2 127.0.0.2 >/dev/null 2>&1");

    CHECK(prc == 0, "specific-only: unmatched traffic passes (ping 127.0.0.2)");
    r1->active = FALSE;
    rc = tc_apply_interface(&p, "lo", err, sizeof err);
    CHECK(rc == 0, "specific-only: clear");
    state = tc_get_state_text("lo");
    CHECK(strstr(state, "netem") == NULL, "specific-only: cleared");
    g_free(state);

    /* ---- batch mode ----
     * The whole tree is applied by one tc process: unprivileged, each command
     * run separately was its own sudo, i.e. its own PAM/logind session. */
    const char *batch_ok[] = {
        "qdisc add dev lo root handle 1: htb default 1",
        "class add dev lo parent 1: classid 1:1 htb rate 100gbit ceil 100gbit",
        "qdisc add dev lo parent 1:1 netem delay 42ms",
    };
    char bout[4096];
    CHECK(tc_run_batch(batch_ok, 3, bout, sizeof bout) == 0, "batch applies a whole tree");
    state = tc_get_state_text("lo");
    CHECK(strstr(state, "42ms") != NULL, "batch: the tree is really there");
    g_free(state);
    const char *batch_bad[] = { "qdisc add dev lo parent 1:99 netem delay 1ms" };
    CHECK(tc_run_batch(batch_bad, 1, bout, sizeof bout) != 0, "batch reports a failed command");
    /* a newline would open a batch line of its own: refused before any exec */
    const char *batch_inj[] = { "qdisc show dev lo\nqdisc del dev lo root" };
    CHECK(tc_run_batch(batch_inj, 1, bout, sizeof bout) == -1, "batch rejects an embedded newline");
    state = tc_get_state_text("lo");
    CHECK(strstr(state, "42ms") != NULL, "batch: the injected del did not run");
    g_free(state);
    CHECK(tc_clear_interface("lo", err, sizeof err) == 0, "batch: clear");

    /* ---- interface list: the unit of work for a bulk start/stop ----
     * start/stop all must converge each interface once, not once per rule;
     * per-rule rebuilds are quadratic and every tc command is its own sudo. */
    r1->active = TRUE;
    r2->active = TRUE;
    GPtrArray *pifs = tc_profile_ifaces(&p, TRUE);
    CHECK(pifs->len == 1, "two rules on one interface list it once (got %u)", pifs->len);
    CHECK(pifs->len == 1 &&
          g_strcmp0((const char *)g_ptr_array_index(pifs, 0), "lo") == 0,
          "profile interface is lo");
    g_ptr_array_free(pifs, TRUE);
    r1->active = FALSE;
    r2->active = FALSE;
    pifs = tc_profile_ifaces(&p, TRUE);
    CHECK(pifs->len == 0, "no active rules, no interfaces to converge");
    g_ptr_array_free(pifs, TRUE);
    pifs = tc_profile_ifaces(&p, FALSE);
    CHECK(pifs->len == 1, "inactive rules still name their interface");
    g_ptr_array_free(pifs, TRUE);

    /* ---- active-state reconciliation ----
     * A profile carries the `active` flags it was saved with, but tc state
     * lives in the kernel: after a reboot the flags are stale. Loading them
     * as-is showed rules as active with nothing applied, and Start is a no-op
     * on an already-active rule, so latency changes did nothing. */
    r1->active = TRUE;
    r2->active = TRUE;
    int stale = tc_sync_active_state(&p);   /* lo is clean at this point */
    CHECK(stale == 2, "sync clears flags when the interface is not shaped (got %d)", stale);
    CHECK(!r1->active && !r2->active, "sync: both rules marked stopped");

    r2->active = TRUE;                      /* all-traffic rule, really applied */
    rc = tc_apply_interface(&p, "lo", err, sizeof err);
    CHECK(rc == 0, "sync: re-apply for the live-state check (%s)", err);
    r1->active = TRUE;                      /* flag set without re-applying */
    CHECK(tc_sync_active_state(&p) == 0, "sync keeps flags while we are shaping");
    CHECK(r1->active && r2->active, "sync: live rules stay active");
    r1->active = FALSE;
    r2->active = FALSE;
    rc = tc_apply_interface(&p, "lo", err, sizeof err);
    CHECK(rc == 0, "sync: clear after the live-state check");

    /* ---- profile round-trip ---- */
    const char *path = "/tmp/lag_test_profile.json";
    unlink(path);
    rc = profile_save(&p, path, err, sizeof err);
    CHECK(rc == 0, "profile save");
    if (rc == 0) {
        FILE *f = fopen(path, "r");
        if (f) {
            printf("PROFILE FILE:\n");
            char c;
            while ((c = fgetc(f)) != EOF) fputc(c, stdout);
            fclose(f);
        }
    }
    LagProfile p2;
    profile_clear(&p2);
    rc = profile_load(&p2, path, err, sizeof err);
    CHECK(rc == 0, "profile load (%s)", err);
    CHECK(p2.count == 2, "loaded %d rules (want 2)", p2.count);
    if (p2.count == 2) {
        CHECK(g_strcmp0(p2.rules[0].iface, "lo") == 0, "rt iface");
        CHECK(g_strcmp0(p2.rules[0].dst_ip, "127.0.0.1") == 0, "rt ip");
        CHECK(g_strcmp0(p2.rules[0].dst_port, "443") == 0, "rt port");
        CHECK(g_strcmp0(p2.rules[0].bandwidth, "100kbit") == 0, "rt bandwidth");
        CHECK(p2.rules[0].latency_ms == 100.0, "rt latency");
        CHECK(p2.rules[0].jitter_ms == 20.0, "rt jitter");
        CHECK(p2.rules[0].drop_pct == 5.0, "rt drop");
        CHECK(p2.rules[1].dst_ip[0] == '\0' && p2.rules[1].dst_port[0] == '\0',
              "rt all-traffic rule");
        CHECK(p2.rules[1].latency_ms == 300.0, "rt all-traffic latency");
    }
    unlink(path);

    /* ---- a profile file is untrusted input ---- */
    const char *badpath = "/tmp/lag_test_bad_profile.json";
    FILE *bf = fopen(badpath, "w");
    if (bf) {
        fputs("{\"version\":1,\"rules\":["
              "{\"iface\":\"lo;touch /tmp/lag_test_pwned\",\"latency_ms\":10},"
              "{\"iface\":\"lo\",\"ip\":\"1.2.3.4;reboot\",\"latency_ms\":10},"
              "{\"iface\":\"lo\",\"bandwidth\":\"1mbit;id\",\"latency_ms\":10},"
              "{\"iface\":\"lo\",\"latency_ms\":1e300},"
              "{\"iface\":\"lo\",\"latency_ms\":42}]}", bf);
        fclose(bf);
        LagProfile p3;
        profile_clear(&p3);
        unlink("/tmp/lag_test_pwned");
        rc = profile_load(&p3, badpath, err, sizeof err);
        printf("hostile profile load: rc=%d warn=%s\n", rc, err);
        CHECK(rc == 0, "hostile profile still loads");
        CHECK(p3.count == 1, "only the 1 valid rule kept (got %d)", p3.count);
        CHECK(p3.count == 1 && p3.rules[0].latency_ms == 42.0, "the kept rule is the good one");
        CHECK(err[0] != '\0', "dropped rules are reported");
        CHECK(access("/tmp/lag_test_pwned", F_OK) != 0, "nothing was executed");
        unlink(badpath);
    }

    /* deeply nested JSON must not recurse the stack away */
    const char *deeppath = "/tmp/lag_test_deep_profile.json";
    FILE *df = fopen(deeppath, "w");
    if (df) {
        fputs("{\"x\":", df);
        for (int k = 0; k < 200000; k++) fputc('[', df);
        for (int k = 0; k < 200000; k++) fputc(']', df);
        fputs("}", df);
        fclose(df);
        LagProfile p4;
        profile_clear(&p4);
        rc = profile_load(&p4, deeppath, err, sizeof err);
        CHECK(rc == -1, "deeply nested profile rejected, not a stack overflow");
        unlink(deeppath);
    }

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
