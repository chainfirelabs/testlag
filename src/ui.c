/* GTK3 front-end for lag: rule list, rule form, start/stop actions,
 * live tc state panel, log, and profile save/open. */
#include "ui.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "profile.h"
#include "tc.h"

enum {
    COL_IFACE, COL_IP, COL_PORT, COL_BW, COL_LAT, COL_JIT, COL_DROP,
    COL_STATUS, N_COLS
};

typedef struct {
    LagProfile *profile;
    char *profile_path;      /* owned by the UI */
    guint state_timer;       /* refresh source; removed before ui is freed */

    GtkWidget    *window;
    GtkListStore *store;
    GtkWidget    *tree;

    /* form */
    GtkWidget *iface_combo;
    GtkWidget *ip_entry;
    GtkWidget *port_entry;
    GtkWidget *bw_entry;
    GtkWidget *latency_spin;
    GtkWidget *jitter_spin;
    GtkWidget *drop_spin;

    /* panels */
    GtkWidget *state_view;
    GtkWidget *log_view;
    GtkWidget *priv_label;
    GtkWidget *sudo_check;
    GtkWidget *path_label;
} UI;

/* ---------------- small helpers ---------------- */

static void fmt_ms(char *buf, size_t n, double v) {
    if (v == (double)(long long)v)
        g_snprintf(buf, n, "%lld ms", (long long)v);
    else
        g_snprintf(buf, n, "%g ms", v);
}

static void ui_log(UI *ui, const char *fmt, ...) {
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui->log_view));
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%H:%M:%S", &tm);

    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    char *line = g_strdup_printf("[%s] %s\n", ts, msg);
    GtkTextIter end_iter;
    gtk_text_buffer_get_end_iter(buf, &end_iter);
    gtk_text_buffer_insert(buf, &end_iter, line, -1);
    g_free(line);
    g_free(msg);

    /* keep the log bounded and scrolled to the end */
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    gint nchars = gtk_text_iter_get_offset(&end);
    if (nchars > 20000) {
        gtk_text_iter_set_offset(&start, nchars - 20000);
        gtk_text_buffer_delete(buf, &start, &end);
    }
    gtk_text_buffer_get_end_iter(buf, &end);
    GtkTextMark *mark = gtk_text_buffer_create_mark(buf, NULL, &end, FALSE);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(ui->log_view), mark, 0.0, TRUE, 0, 0);
    gtk_text_buffer_delete_mark(buf, mark);
}

static void show_error(UI *ui, const char *title, const char *msg) {
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(ui->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s", msg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    ui_log(ui, "ERROR: %s", msg);
}

static void ui_refresh_list(UI *ui) {
    gtk_list_store_clear(ui->store);
    for (int i = 0; i < ui->profile->count; i++) {
        const LagRule *r = &ui->profile->rules[i];
        GtkTreeIter it;
        gtk_list_store_append(ui->store, &it);
        char lat[48], jit[48], drop[48];
        fmt_ms(lat, sizeof lat, r->latency_ms);
        fmt_ms(jit, sizeof jit, r->jitter_ms);
        g_snprintf(drop, sizeof drop, "%g%%", r->drop_pct);
        gtk_list_store_set(ui->store, &it,
            COL_IFACE,  r->iface,
            COL_IP,     r->ip[0] ? r->ip : "(all)",
            COL_PORT,   r->port[0] ? r->port : "(all)",
            COL_BW,     r->bandwidth[0] ? r->bandwidth : "(none)",
            COL_LAT,    lat,
            COL_JIT,    jit,
            COL_DROP,   drop,
            COL_STATUS, r->active ? "Active" : "Stopped",
            -1);
    }
}

static int ui_selected_index(UI *ui) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree));
    GtkTreeIter it;
    GtkTreeModel *model;
    if (gtk_tree_selection_get_selected(sel, &model, &it)) {
        GtkTreePath *path = gtk_tree_model_get_path(model, &it);
        char *str = gtk_tree_path_to_string(path);
        int idx = atoi(str);
        g_free(str);
        gtk_tree_path_free(path);
        return idx;
    }
    return -1;
}

static void clear_form(UI *ui) {
    gtk_entry_set_text(GTK_ENTRY(ui->ip_entry), "");
    gtk_entry_set_text(GTK_ENTRY(ui->port_entry), "");
    gtk_entry_set_text(GTK_ENTRY(ui->bw_entry), "");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->latency_spin), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->jitter_spin), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->drop_spin), 0);
}

static void load_form(UI *ui, const LagRule *r) {
    /* select iface in combo if present */
    if (r->iface[0]) {
        GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(ui->iface_combo));
        int n = gtk_tree_model_iter_n_children(model, NULL);
        for (int i = 0; i < n; i++) {
            GtkTreeIter it;
            if (!gtk_tree_model_iter_nth_child(model, &it, NULL, i))
                continue;
            char *t = NULL;
            gtk_tree_model_get(model, &it, 0, &t, -1);
            gboolean match = (g_strcmp0(t, r->iface) == 0);
            g_free(t);
            if (match) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(ui->iface_combo), i);
                return;
            }
        }
        gtk_combo_box_text_insert_text(GTK_COMBO_BOX_TEXT(ui->iface_combo), -1, r->iface);
        gtk_combo_box_set_active(GTK_COMBO_BOX(ui->iface_combo), n);
    }
    gtk_entry_set_text(GTK_ENTRY(ui->ip_entry), r->ip);
    gtk_entry_set_text(GTK_ENTRY(ui->port_entry), r->port);
    gtk_entry_set_text(GTK_ENTRY(ui->bw_entry), r->bandwidth);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->latency_spin), r->latency_ms);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->jitter_spin), r->jitter_ms);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->drop_spin), r->drop_pct);
}

static gboolean form_get_rule(UI *ui, LagRule *r, char *err, size_t errlen) {
    lag_rule_reset(r);
    /* newly allocated by GTK: must be freed on every path out of here */
    char *iface = gtk_combo_box_text_get_active_text(
        GTK_COMBO_BOX_TEXT(ui->iface_combo));
    if (!iface || !*iface) {
        g_snprintf(err, errlen, "Interface is required.");
        g_free(iface);
        return FALSE;
    }
    if (!tc_valid_iface(iface)) {
        g_snprintf(err, errlen, "Invalid interface name: %s", iface);
        g_free(iface);
        return FALSE;
    }
    g_strlcpy(r->iface, iface, LAG_IFACE_LEN);
    g_free(iface);

    const char *ip = gtk_entry_get_text(GTK_ENTRY(ui->ip_entry));
    if (ip && *ip) {
        if (!tc_valid_ip(ip)) {
            g_snprintf(err, errlen, "Invalid IP address: %s", ip);
            return FALSE;
        }
        g_strlcpy(r->ip, ip, LAG_IP_LEN);
    }

    const char *port = gtk_entry_get_text(GTK_ENTRY(ui->port_entry));
    if (port && *port) {
        if (!tc_valid_port(port)) {
            g_snprintf(err, errlen, "Invalid port: %s (must be 1-65535)", port);
            return FALSE;
        }
        g_strlcpy(r->port, port, LAG_PORT_LEN);
    }

    const char *bw = gtk_entry_get_text(GTK_ENTRY(ui->bw_entry));
    if (bw && *bw) {
        if (!tc_valid_rate(bw)) {
            g_snprintf(err, errlen,
                "Invalid bandwidth: %s (use e.g. 100kbit, 1mbit, 1.5mbit)", bw);
            return FALSE;
        }
        g_strlcpy(r->bandwidth, bw, LAG_BW_LEN);
    }

    r->latency_ms = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui->latency_spin));
    r->jitter_ms  = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui->jitter_spin));
    r->drop_pct   = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui->drop_spin));

    if (r->latency_ms <= 0 && r->jitter_ms <= 0 && r->drop_pct <= 0
        && r->bandwidth[0] == '\0') {
        g_snprintf(err, errlen,
            "Set at least one effect: latency, jitter, drop %% or bandwidth.");
        return FALSE;
    }
    return tc_valid_rule(r, err, errlen);
}

/* ---------------- actions ---------------- */

static void refresh_state_now(UI *ui) {
    char ifaces[LAG_MAX_RULES][LAG_IFACE_LEN];
    int n = 0;
    for (int i = 0; i < ui->profile->count; i++) {
        const char *ifc = ui->profile->rules[i].iface;
        if (!ifc[0]) continue;
        int dup = 0;
        for (int k = 0; k < n; k++)
            if (g_strcmp0(ifaces[k], ifc) == 0) { dup = 1; break; }
        if (!dup && n < LAG_MAX_RULES) {
            g_strlcpy(ifaces[n], ifc, LAG_IFACE_LEN);
            n++;
        }
    }
    GString *s = g_string_new("");
    for (int k = 0; k < n; k++) {
        char *txt = tc_get_state_text(ifaces[k]);
        g_string_append_printf(s, "=== %s ===\n%s\n\n", ifaces[k], txt);
        g_free(txt);
    }
    if (s->len == 0)
        g_string_append(s, "(no rules defined)\n");
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui->state_view));
    gtk_text_buffer_set_text(buf, s->str, -1);
    g_string_free(s, TRUE);
}

static gboolean refresh_state_timer(gpointer data) {
    refresh_state_now((UI *)data);
    return G_SOURCE_CONTINUE;
}

static void do_start(UI *ui, int idx) {
    if (idx < 0 || idx >= ui->profile->count) {
        show_error(ui, "No rule selected", "Select a rule in the list first.");
        return;
    }
    LagRule *r = &ui->profile->rules[idx];
    if (r->active)
        return;
    char err[2048];
    r->active = TRUE;
    if (tc_apply_interface(ui->profile, r->iface, err, sizeof err) != 0) {
        r->active = FALSE;
        ui_refresh_list(ui);
        refresh_state_now(ui);
        show_error(ui, "Start failed", err);
        return;
    }
    ui_log(ui, "Started rule on %s (ip=%s port=%s lat=%.3gms jit=%.3gms drop=%.3g%% bw=%s)",
        r->iface, r->ip[0] ? r->ip : "any", r->port[0] ? r->port : "any",
        r->latency_ms, r->jitter_ms, r->drop_pct,
        r->bandwidth[0] ? r->bandwidth : "unlimited");
    ui_refresh_list(ui);
    refresh_state_now(ui);
}

static void do_stop(UI *ui, int idx) {
    if (idx < 0 || idx >= ui->profile->count) {
        show_error(ui, "No rule selected", "Select a rule in the list first.");
        return;
    }
    LagRule *r = &ui->profile->rules[idx];
    if (!r->active)
        return;
    r->active = FALSE;
    char err[2048];
    if (tc_apply_interface(ui->profile, r->iface, err, sizeof err) != 0) {
        ui_log(ui, "WARNING: stop on %s: %s", r->iface, err);
    } else {
        ui_log(ui, "Stopped rule on %s", r->iface);
    }
    ui_refresh_list(ui);
    refresh_state_now(ui);
}

static void on_add_clicked(GtkButton *b, gpointer data) {
    (void)b;
    UI *ui = data;
    if (ui->profile->count >= LAG_MAX_RULES) {
        char msg[128];
        g_snprintf(msg, sizeof msg, "Maximum of %d rules.", LAG_MAX_RULES);
        show_error(ui, "Rule limit reached", msg);
        return;
    }
    LagRule r;
    char err[512];
    if (!form_get_rule(ui, &r, err, sizeof err)) {
        show_error(ui, "Invalid rule", err);
        return;
    }
    ui->profile->rules[ui->profile->count++] = r;
    ui_log(ui, "Added rule on %s", r.iface);
    ui_refresh_list(ui);
    clear_form(ui);
}

static void on_edit_clicked(GtkButton *b, gpointer data) {
    (void)b;
    UI *ui = data;
    int idx = ui_selected_index(ui);
    if (idx < 0) {
        show_error(ui, "No rule selected", "Select a rule in the list to edit.");
        return;
    }
    LagRule r;
    char verr[512];
    if (!form_get_rule(ui, &r, verr, sizeof verr)) {
        show_error(ui, "Invalid rule", verr);
        return;
    }
    LagRule *old = &ui->profile->rules[idx];
    char old_iface[LAG_IFACE_LEN];
    g_strlcpy(old_iface, old->iface, sizeof old_iface);
    r.active = old->active;
    ui->profile->rules[idx] = r;
    if (r.active) {
        char e2[2048];
        if (g_strcmp0(old_iface, r.iface) != 0)
            tc_apply_interface(ui->profile, old_iface, e2, sizeof e2);
        if (tc_apply_interface(ui->profile, r.iface, e2, sizeof e2) != 0) {
            ui->profile->rules[idx].active = FALSE;
            show_error(ui, "Edit failed (rule deactivated)", e2);
        } else {
            ui_log(ui, "Updated active rule on %s", r.iface);
        }
    } else {
        ui_log(ui, "Updated rule on %s", r.iface);
    }
    ui_refresh_list(ui);
    refresh_state_now(ui);
}

static void on_delete_clicked(GtkButton *b, gpointer data) {
    (void)b;
    UI *ui = data;
    int idx = ui_selected_index(ui);
    if (idx < 0) {
        show_error(ui, "No rule selected", "Select a rule in the list to delete.");
        return;
    }
    LagRule *r = &ui->profile->rules[idx];
    if (r->active) {
        r->active = FALSE;
        char err[2048];
        tc_apply_interface(ui->profile, r->iface, err, sizeof err);
        ui_log(ui, "Deleted active rule on %s (stopped first)", r->iface);
    } else {
        ui_log(ui, "Deleted rule on %s", r->iface);
    }
    for (int i = idx; i < ui->profile->count - 1; i++)
        ui->profile->rules[i] = ui->profile->rules[i + 1];
    ui->profile->count--;
    ui_refresh_list(ui);
    refresh_state_now(ui);
}

static void on_start_clicked(GtkButton *b, gpointer data) {
    (void)b;
    do_start((UI *)data, ui_selected_index((UI *)data));
}

static void on_stop_clicked(GtkButton *b, gpointer data) {
    (void)b;
    do_stop((UI *)data, ui_selected_index((UI *)data));
}

static void on_start_all_clicked(GtkButton *b, gpointer data) {
    (void)b;
    UI *ui = data;
    for (int i = 0; i < ui->profile->count; i++)
        if (!ui->profile->rules[i].active)
            do_start(ui, i);
}

static void on_stop_all_clicked(GtkButton *b, gpointer data) {
    (void)b;
    UI *ui = data;
    for (int i = ui->profile->count - 1; i >= 0; i--)
        if (ui->profile->rules[i].active)
            do_stop(ui, i);
}

static void on_save_clicked(GtkButton *b, gpointer data) {
    (void)b;
    UI *ui = data;
    char err[512];
    if (profile_save(ui->profile, ui->profile_path, err, sizeof err) != 0) {
        show_error(ui, "Save failed", err);
        return;
    }
    ui_log(ui, "Profile saved to %s", ui->profile_path);
}

static void on_open_clicked(GtkButton *b, gpointer data) {
    (void)b;
    UI *ui = data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Open profile",
        GTK_WINDOW(ui->window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    if (resp == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        char err[512];
        LagProfile tmp;
        profile_clear(&tmp);
        if (profile_load(&tmp, path, err, sizeof err) == 0) {
            *ui->profile = tmp;
            g_free(ui->profile_path);
            ui->profile_path = path;   /* the UI owns it from here on */
            gtk_label_set_text(GTK_LABEL(ui->path_label), path);
            ui_refresh_list(ui);
            refresh_state_now(ui);
            ui_log(ui, "Profile loaded from %s (%d rules)", path, tmp.count);
            if (err[0])
                show_error(ui, "Some rules were not loaded", err);
        } else {
            g_free(path);
            show_error(ui, "Open failed", err);
        }
    }
    gtk_widget_destroy(dlg);
}

static void on_sudo_toggled(GtkToggleButton *btn, gpointer data) {
    UI *ui = data;
    tc_set_use_sudo(gtk_toggle_button_get_active(btn));
    ui_log(ui, "sudo for tc writes: %s",
        gtk_toggle_button_get_active(btn) ? "on" : "off");
}

static void on_tree_row_activated(GtkTreeView *tree, GtkTreePath *path,
                                  GtkTreeViewColumn *col, gpointer data) {
    (void)tree; (void)path; (void)col;
    /* the double-clicked row is already the selected one */
    UI *ui = data;
    do_start(ui, ui_selected_index(ui));
}

static void on_row_selected(GtkTreeSelection *sel, gpointer data) {
    (void)sel;
    UI *ui = data;
    int idx = ui_selected_index(ui);
    if (idx >= 0)
        load_form(ui, &ui->profile->rules[idx]);
}

static void on_destroy(GtkWidget *w, gpointer data) {
    (void)w;
    UI *ui = data;
    /* the timer holds ui and touches widgets that are being destroyed */
    if (ui->state_timer) {
        g_source_remove(ui->state_timer);
        ui->state_timer = 0;
    }
    char err[512];
    if (profile_save(ui->profile, ui->profile_path, err, sizeof err) != 0) {
        g_warning("Could not save profile on exit: %s", err);
    } else {
        ui_log(ui, "Profile saved to %s", ui->profile_path);
    }
    gtk_main_quit();
}

/* ---------------- widget construction ---------------- */

static GtkWidget *make_label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    return l;
}

static GtkWidget *make_spin(double lo, double hi, double step) {
    GtkWidget *s = gtk_spin_button_new_with_range(lo, hi, step);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(s), 1);
    return s;
}

static void build_tree(UI *ui, GtkWidget *vbox) {
    ui->store = gtk_list_store_new(N_COLS,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

    ui->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ui->store));
    gtk_tree_view_set_reorderable(GTK_TREE_VIEW(ui->tree), FALSE);

    const char *titles[N_COLS] = {
        "Interface", "IP", "Port", "Bandwidth",
        "Latency", "Jitter", "Drop", "Status"
    };
    for (int c = 0; c < N_COLS; c++) {
        GtkCellRenderer *rend = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *col =
            gtk_tree_view_column_new_with_attributes(titles[c], rend,
                "text", c, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(ui->tree), col);
    }

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 160);
    gtk_container_add(GTK_CONTAINER(scroll), ui->tree);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree));
    g_signal_connect(sel, "changed", G_CALLBACK(on_row_selected), ui);
    g_signal_connect(ui->tree, "row-activated",
        G_CALLBACK(on_tree_row_activated), ui);
}

static void build_form(UI *ui, GtkWidget *vbox) {
    GtkWidget *frame = gtk_frame_new("Rule");
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
    gtk_container_add(GTK_CONTAINER(frame), grid);
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);

    ui->iface_combo = gtk_combo_box_text_new_with_entry();
    ui->ip_entry    = gtk_entry_new();
    ui->port_entry  = gtk_entry_new();
    ui->bw_entry    = gtk_entry_new();
    ui->latency_spin = make_spin(0, 60000, 10);
    ui->jitter_spin  = make_spin(0, 60000, 1);
    ui->drop_spin    = make_spin(0, 100, 1);

    gtk_entry_set_placeholder_text(GTK_ENTRY(ui->ip_entry), "IP (blank = all)");
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui->port_entry), "port (blank = all)");
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui->bw_entry), "bandwidth e.g. 100kbit (blank = unlimited)");

    int col = 0;
    gtk_grid_attach(GTK_GRID(grid), make_label("Interface"), col++, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui->iface_combo, col++, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_label("Destination IP"), col++, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui->ip_entry, col++, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_label("Port"), col++, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui->port_entry, col++, 0, 1, 1);

    col = 0;
    gtk_grid_attach(GTK_GRID(grid), make_label("Bandwidth"), col++, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui->bw_entry, col++, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_label("Latency (ms)"), col++, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui->latency_spin, col++, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_label("Jitter (ms)"), col++, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui->jitter_spin, col++, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_label("Drop (%)"), col++, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ui->drop_spin, col++, 1, 1, 1);

    /* action buttons — each wired to its callback */
    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b_add       = gtk_button_new_with_label("Add");
    GtkWidget *b_edit      = gtk_button_new_with_label("Edit selected");
    GtkWidget *b_del       = gtk_button_new_with_label("Delete selected");
    GtkWidget *b_start     = gtk_button_new_with_label("Start");
    GtkWidget *b_stop      = gtk_button_new_with_label("Stop");
    GtkWidget *b_start_all = gtk_button_new_with_label("Start all");
    GtkWidget *b_stop_all  = gtk_button_new_with_label("Stop all");

    gtk_box_pack_start(GTK_BOX(btns), b_add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), b_edit, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), b_del, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), b_start, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), b_stop, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), b_start_all, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), b_stop_all, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), btns, 0, 2, N_COLS, 1);

    g_signal_connect(b_add,       "clicked", G_CALLBACK(on_add_clicked),       ui);
    g_signal_connect(b_edit,      "clicked", G_CALLBACK(on_edit_clicked),      ui);
    g_signal_connect(b_del,       "clicked", G_CALLBACK(on_delete_clicked),    ui);
    g_signal_connect(b_start,     "clicked", G_CALLBACK(on_start_clicked),     ui);
    g_signal_connect(b_stop,      "clicked", G_CALLBACK(on_stop_clicked),      ui);
    g_signal_connect(b_start_all, "clicked", G_CALLBACK(on_start_all_clicked), ui);
    g_signal_connect(b_stop_all,  "clicked", G_CALLBACK(on_stop_all_clicked),  ui);
}

/* ---- theme: dark charcoal + orange, matching logo-banner.png ---- */

static const char *LAG_CSS =
    /* palette (sampled from logo-banner.png) */
    "@define-color lag_bg        #141414;"
    "@define-color lag_bg2       #1e1e1e;"
    "@define-color lag_bg3       #262626;"
    "@define-color lag_fg        #e8e8e8;"
    "@define-color lag_fg_dim    #9a9a9a;"
    "@define-color lag_accent    #c94e24;"
    "@define-color lag_accent2   #f7941d;"
    "@define-color lag_accent_dk #a4401f;"
    "@define-color lag_border    #3a3a3a;"
    /* base */
    "window, .background {"
    "  background-image: none; background-color: @lag_bg; color: @lag_fg;"
    "  border: none; }"
    "label { color: @lag_fg; }"
    ".dim-label, label:disabled { color: @lag_fg_dim; }"
    "separator { background-color: @lag_border; }"
    "decoration {"
    "  background-color: @lag_bg; background-image: none; border: none; }"
    /* header / titlebar */
    "headerbar, .titlebar {"
    "  background-image: none; background-color: @lag_bg2; color: @lag_fg;"
    "  border: none; border-bottom: 1px solid @lag_border;"
    "  box-shadow: none }"
    "headerbar button {"
    "  background-color: @lag_bg3; color: @lag_fg;"
    "  border: 1px solid @lag_border; border-radius: 4px; }"
    "headerbar button:hover { background-color: @lag_accent; color: #ffffff;"
    "  border-color: @lag_accent; }"
    /* buttons: orange primary */
    "button {"
    "  background-image: none; background-color: @lag_accent;"
    "  color: #ffffff; border: 1px solid @lag_accent_dk;"
    "  border-radius: 4px; padding: 4px 12px; box-shadow: none }"
    "button:hover  { background-image: none; background-color: @lag_accent2;"
    "  border-color: @lag_accent2; box-shadow: none }"
    "button:active, button:checked { background-image: none;"
    "  background-color: @lag_accent_dk; box-shadow: none }"
    "button:disabled { background-image: none; background-color: @lag_bg3;"
    "  color: @lag_fg_dim; border-color: @lag_border; box-shadow: none }"
    /* entries / spinbuttons / combo */
    "entry, spinbutton, combobox entry {"
    "  background-image: none; background-color: @lag_bg3; color: @lag_fg;"
    "  border: 1px solid @lag_border; border-radius: 4px; }"
    "entry:focus, spinbutton:focus, combobox entry:focus {"
    "  border-color: @lag_accent2; }"
    "entry selection, spinbutton selection {"
    "  background-color: @lag_accent; color: #ffffff; }"
    "combobox, combobox button {"
    "  background-image: none; background-color: @lag_bg3; color: @lag_fg;"
    "  border: 1px solid @lag_border; border-radius: 4px; }"
    "combobox button:hover { background-color: @lag_accent; color: #ffffff; }"
    "combobox arrow { color: @lag_fg; }"
    "combobox window.background, popover {"
    "  background-image: none; background-color: @lag_bg2; color: @lag_fg; }"
    /* combo dropdown is a GtkTreeMenu: menu/menuitem nodes, not treeview */
    "menu, menu menuitem {"
    "  background-image: none; background-color: @lag_bg2; color: @lag_fg; }"
    "menu menuitem:hover, menu menuitem:selected {"
    "  background-image: none; background-color: @lag_accent; color: #ffffff; }"
    /* pin menu cell text color directly (defense: a theme rule on cellview
       would beat the color inherited from menuitem) */
    "cellview { color: @lag_fg; }"
    "menu menuitem:hover cellview, menu menuitem:selected cellview {"
    "  color: #ffffff; }"
    /* rule list (treeview) */
    "treeview { background-image: none; background-color: @lag_bg; color: @lag_fg; }"
    "treeview:selected {"
    "  background-image: none; background-color: @lag_accent; color: #ffffff; }"
    "treeview header button {"
    "  background-image: none; background-color: @lag_bg2; color: @lag_fg;"
    "  border: 1px solid @lag_border; }"
    "treeview header button:hover { background-color: @lag_accent; color: #ffffff; }"
    /* frames + text views */
    "frame, frame > border {"
    "  border: 1px solid @lag_border; background-image: none;"
    "  background-color: transparent; }"
    "frame > label { color: @lag_accent2; background-image: none;"
    "  background-color: transparent; border: none; box-shadow: none }"
    "textview, textview text {"
    "  background-image: none; background-color: @lag_bg2; color: @lag_fg; }"
    "textview text selection { background-color: @lag_accent; color: #ffffff; }"
    /* checkbutton (sudo toggle) */
    "checkbutton { color: @lag_fg; }"
    "checkbutton check {"
    "  background-image: none; background-color: @lag_bg3; color: @lag_fg;"
    "  border: 1px solid @lag_border; border-radius: 3px; }"
    "checkbutton check:checked {"
    "  background-color: @lag_accent; border-color: @lag_accent; color: #ffffff; }"
    "checkbutton check:hover { border-color: @lag_accent2; }"
    /* scrollbars */
    "scrollbar, scrollbar trough {"
    "  background-image: none; background-color: @lag_bg; border: none; }"
    "scrollbar slider {"
    "  background-color: @lag_border; border-radius: 4px; }"
    "scrollbar slider:hover { background-color: @lag_accent; }"
    /* tooltips + dialogs */
    "tooltip, tooltip * { background-image: none; background-color: @lag_bg2; color: @lag_fg; }"
    "dialog, .dialog, .message-area {"
    "  background-image: none; background-color: @lag_bg; color: @lag_fg; }"
    "dialog button { background-color: @lag_accent; color: #ffffff; }";

static void apply_theme(void) {
    GtkCssProvider *prov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(prov, LAG_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(prov);
}

/* locate logo-banner.png next to the executable, falling back to CWD */
static char *find_logo_path(void) {
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *dir = g_path_get_dirname(exe);
        char *p = g_build_filename(dir, "logo-banner.png", NULL);
        g_free(dir);

        if (g_file_test(p, G_FILE_TEST_IS_REGULAR))
            return p;
        g_free(p);
    }
    return g_strdup("logo-banner.png");
}

void ui_run(LagProfile *profile, const char *profile_path, gboolean use_sudo) {
    tc_set_use_sudo(use_sudo);
    apply_theme();

    UI *ui = g_new0(UI, 1);
    ui->profile = profile;
    /* our own copy: "Open profile..." replaces it, and the caller still owns
       (and frees) the string it passed in */
    ui->profile_path = g_strdup(profile_path);

    ui->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(ui->window), "TestLag — tc latency/loss shaper");
    gtk_window_set_default_size(GTK_WINDOW(ui->window), 980, 720);
    char *logo = find_logo_path();
    gtk_window_set_icon_from_file(GTK_WINDOW(ui->window), logo, NULL);
    g_signal_connect(ui->window, "destroy", G_CALLBACK(on_destroy), ui);

    /* CSD titlebar: logo icon upper-left, title, close button */
    GtkWidget *titlebar = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(titlebar), TRUE);
    GdkPixbuf *tlogo_pb =
        gdk_pixbuf_new_from_file_at_scale(logo, -1, 24, TRUE, NULL);
    if (tlogo_pb) {
        gtk_header_bar_pack_start(GTK_HEADER_BAR(titlebar),
            gtk_image_new_from_pixbuf(tlogo_pb));
        g_object_unref(tlogo_pb);
    }
    GtkWidget *tlabel = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(tlabel),
        "<span foreground='#f7941d'><b>TestLag</b></span>");
    gtk_header_bar_set_custom_title(GTK_HEADER_BAR(titlebar), tlabel);
    gtk_window_set_titlebar(GTK_WINDOW(ui->window), titlebar);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(ui->window), vbox);

    /* header: title + privilege */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title),
        "<span foreground='#f7941d'><b>TestLag</b></span>"
        "  —  per-IP/port latency, jitter, loss &amp; bandwidth via tc/netem");
    gtk_box_pack_start(GTK_BOX(header), title, TRUE, TRUE, 0);
    ui->priv_label = gtk_label_new("");
    gtk_box_pack_end(GTK_BOX(header), ui->priv_label, FALSE, FALSE, 0);
    ui->sudo_check = gtk_check_button_new_with_label("use sudo for tc writes");
    gtk_box_pack_end(GTK_BOX(header), ui->sudo_check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), header, FALSE, FALSE, 0);

    if (tc_is_root()) {
        gtk_label_set_text(GTK_LABEL(ui->priv_label),
            "Running as root — tc runs directly.");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->sudo_check), FALSE);
        gtk_widget_set_sensitive(ui->sudo_check, FALSE);
    } else {
        gtk_label_set_text(GTK_LABEL(ui->priv_label),
            "Unprivileged — changes need sudo (password may be requested).");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->sudo_check), TRUE);
    }
    g_signal_connect(ui->sudo_check, "toggled", G_CALLBACK(on_sudo_toggled), ui);

    /* rule list */
    build_tree(ui, vbox);

    /* form */
    build_form(ui, vbox);

    /* profile row */
    GtkWidget *prow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *b_save = gtk_button_new_with_label("Save profile");
    GtkWidget *b_open = gtk_button_new_with_label("Open profile…");
    gtk_box_pack_start(GTK_BOX(prow), b_save, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(prow), b_open, FALSE, FALSE, 0);
    ui->path_label = gtk_label_new(profile_path);
    gtk_widget_set_halign(ui->path_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(prow), ui->path_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), prow, FALSE, FALSE, 0);
    g_signal_connect(b_save, "clicked", G_CALLBACK(on_save_clicked), ui);
    g_signal_connect(b_open, "clicked", G_CALLBACK(on_open_clicked), ui);

    /* live state */
    GtkWidget *sframe = gtk_frame_new("Live tc state (refreshes every 2 s)");
    ui->state_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(ui->state_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(ui->state_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(ui->state_view), TRUE);
    GtkWidget *sscroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sscroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sscroll), 120);
    gtk_container_add(GTK_CONTAINER(sscroll), ui->state_view);
    gtk_container_add(GTK_CONTAINER(sframe), sscroll);
    gtk_box_pack_start(GTK_BOX(vbox), sframe, FALSE, FALSE, 0);

    /* log */
    GtkWidget *lframe = gtk_frame_new("Log");
    ui->log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(ui->log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(ui->log_view), FALSE);
    GtkWidget *lscroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lscroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(lscroll), 100);
    gtk_container_add(GTK_CONTAINER(lscroll), ui->log_view);
    gtk_container_add(GTK_CONTAINER(lframe), lscroll);
    gtk_box_pack_start(GTK_BOX(vbox), lframe, FALSE, FALSE, 0);

    /* populate interface combo */
    GPtrArray *ifs = tc_list_interfaces();
    for (guint i = 0; i < ifs->len; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->iface_combo),
            (char *)g_ptr_array_index(ifs, i));
    if (ifs->len)
        gtk_combo_box_set_active(GTK_COMBO_BOX(ui->iface_combo), 0);
    g_ptr_array_free(ifs, TRUE);

    ui_log(ui, "TestLag started (profile: %s)", profile_path);
    ui_log(ui, "%d rule(s) loaded.", profile->count);
    ui_refresh_list(ui);
    refresh_state_now(ui);
    ui->state_timer = g_timeout_add(2000, refresh_state_timer, ui);

    gtk_widget_show_all(ui->window);
    gtk_main();

    if (ui->state_timer)
        g_source_remove(ui->state_timer);
    g_free(logo);
    g_free(ui->profile_path);
    g_free(ui);
}
