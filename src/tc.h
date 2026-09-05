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

/* Build the netem parameter string for a rule (newly allocated; may be "netem"). */
char *tc_build_netem(const LagRule *r);

/* Build the filter command for a rule (newly allocated) or NULL if all-traffic. */
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
