#ifndef LAG_TC_H
#define LAG_TC_H

#include <glib.h>
#include "lag.h"

/* ---- privilege ---- */
gboolean tc_is_root(void);
gboolean tc_sudo_present(void);
void     tc_set_use_sudo(gboolean use);
gboolean tc_get_use_sudo(void);

/* Run a command, optionally elevated via sudo. The command string is split on
 * whitespace and exec'd directly -- no shell is involved. Combined
 * stdout+stderr is captured into out (if non-NULL, truncated to outlen).
 * Returns the exit status (0 = success) or -1 if the command could not be
 * launched. */
int tc_run(const char *cmd, gboolean needs_priv, char *out, size_t outlen);

/* Run several tc commands in a single privileged process, via tc's own batch
 * mode. `lines` are tc argument lines WITHOUT the leading "tc", e.g.
 * "qdisc add dev lo root handle 1: htb default 1". Still no shell: the batch
 * is data on the child's stdin, never a command line. tc stops at the first
 * command that fails, naming the line in its output.
 * Combined stdout+stderr is captured into out (if non-NULL). Returns the exit
 * status (0 = every command succeeded) or -1 if the child could not run. */
int tc_run_batch(const char *const *lines, int n, char *out, size_t outlen);

/* Network interface names. Caller frees with g_ptr_array_free(arr, TRUE). */
GPtrArray *tc_list_interfaces(void);

/* 0 if iface can be managed (root absent/noqueue, or our htb), else -1 + err. */
int tc_check_interface(const char *iface, char *err, size_t errlen);

/* Rebuild the iface qdisc tree from all active rules in *p.
 * If no rule on iface is active, the root qdisc is removed (clean state).
 * Returns 0 on success, -1 with err filled on failure. */
int tc_apply_interface(LagProfile *p, const char *iface, char *err, size_t errlen);

/* Remove all lag qdiscs/filters from iface (best effort). */
int tc_clear_interface(const char *iface, char *err, size_t errlen);

/* Distinct interface names named by the profile's rules, in first-seen order
 * (active_only: consider only rules that are active). Interfaces are the unit
 * of work for tc_apply_interface(), so callers that touch several rules must
 * converge each interface once rather than once per rule.
 * Caller frees with g_ptr_array_free(arr, TRUE). */
GPtrArray *tc_profile_ifaces(const LagProfile *p, gboolean active_only);

/* Reconcile the profile's `active` flags with the live tc state.
 * A profile records which rules were running when it was saved, but the qdisc
 * tree lives in the kernel, not in the file: after a reboot -- or any time the
 * qdiscs were removed behind our back -- a rule marked active is not applied
 * any more. Rules on an interface we are not currently shaping are marked
 * inactive. Read-only: it inspects tc state and never changes it.
 * Returns the number of rules whose flag was cleared. */
int tc_sync_active_state(LagProfile *p);

/* Build the netem parameter string for a rule (newly allocated; may be "netem"). */
char *tc_build_netem(const LagRule *r);

/* Build the filter line for a rule -- tc arguments without the leading "tc",
 * for tc_run_batch() -- or NULL if the rule is all-traffic. Newly allocated. */
char *tc_build_filter(const LagRule *r, const char *iface, int classid);

/* Human-readable active qdisc/filter state for iface (newly allocated). */
char *tc_get_state_text(const char *iface);

/* ---- input validation ----
 * Applied to every rule that is loaded from a profile, entered in the GUI or
 * CLI, or handed to tc_apply_interface(), whatever its origin. */
gboolean tc_valid_iface(const char *s);
gboolean tc_valid_ip(const char *s);
gboolean tc_valid_port(const char *s);
gboolean tc_valid_rate(const char *s);
gboolean tc_valid_ms(double v);    /* finite, 0 .. LAG_MAX_MS */
gboolean tc_valid_pct(double v);   /* finite, 0 .. 100 */

/* TRUE if every field of the rule is well-formed; otherwise FALSE with a
 * reason written to err (if non-NULL). */
gboolean tc_valid_rule(const LagRule *r, char *err, size_t errlen);

#endif /* LAG_TC_H */
