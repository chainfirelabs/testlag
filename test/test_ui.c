/* SPDX-License-Identifier: MIT */
/* Exercise actual GTK selections/callbacks with network writes intercepted. */
#include "../src/ui.c"

static int calls[4];
static const char *fail_iface;
static gboolean fail_once;
static LagProfile applied[4];
static const char *state_text = "";

int __wrap_tc_apply_interface(LagProfile *p, const char *iface, char *err, size_t n) {
    int slot = strcmp(iface, "lo") == 0 ? 0 : iface[4] - '0';
    g_assert_cmpint(slot, >=, 0);
    g_assert_cmpint(slot, <, 4);
    calls[slot]++;
    applied[slot] = *p;
    if (g_strcmp0(iface, fail_iface) == 0) {
        if (fail_once) fail_iface = NULL;
        g_strlcpy(err, "test: authentication cancelled", n);
        return -1;
    }
    return 0;
}

char *__wrap_tc_get_state_text(const char *iface) {
    (void)iface;
    return g_strdup(state_text);
}

static gboolean dismiss_errors(gpointer data) {
    (void)data;
    GList *windows = gtk_window_list_toplevels();
    for (GList *it = windows; it; it = it->next)
        if (GTK_IS_MESSAGE_DIALOG(it->data))
            gtk_dialog_response(GTK_DIALOG(it->data), GTK_RESPONSE_OK);
    g_list_free(windows);
    return G_SOURCE_CONTINUE;
}

static void select_row(UI *ui, int idx) {
    GtkTreePath *path = gtk_tree_path_new_from_indices(idx, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree)), path);
    gtk_tree_path_free(path);
}

static void reset(UI *ui) {
    gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree)));
    memset(ui->profile, 0, sizeof *ui->profile);
    ui->profile->count = 6;
    for (int i = 0; i < 6; i++) {
        LagRule *r = &ui->profile->rules[i];
        g_snprintf(r->iface, sizeof r->iface, i < 3 ? "lo" : "test%d", i - 2);
        g_snprintf(r->dst_port, sizeof r->dst_port, "%d", 1000 + i);
        r->latency_ms = 10;
    }
    memset(calls, 0, sizeof calls);
    fail_iface = NULL;
    fail_once = FALSE;
    ui_refresh_list(ui);
}

static void settle_output(void) {
    /* Include frame-clock layout and deferred scrolling. */
    gint64 until = g_get_monotonic_time() + 100000;
    do {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    } while (g_get_monotonic_time() < until);
}

static void assert_at_bottom(GtkAdjustment *adjustment) {
    g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(adjustment),
        gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment), 1.0);
}

static void test_output_scrolling(void) {
    LagProfile profile = {.count = 1};
    g_strlcpy(profile.rules[0].iface, "lo", LAG_IFACE_LEN);
    UI ui = {.profile = &profile};
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), box);
    ui.log_view = gtk_text_view_new();
    ui.state_view = gtk_text_view_new();
    GtkWidget *views[] = {ui.log_view, ui.state_view};
    for (int i = 0; i < 2; i++) {
        GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
        gtk_widget_set_size_request(scroller, 400, 120);
        gtk_container_add(GTK_CONTAINER(scroller), views[i]);
        gtk_box_pack_start(GTK_BOX(box), scroller, TRUE, TRUE, 0);
    }
    gtk_widget_show_all(window);
    settle_output();
    GString *text = g_string_new("");
    for (int i = 0; i < 100; i++) g_string_append_printf(text, "Output line %d\n", i);
    state_text = text->str;
    ui_log(&ui, "%s", text->str);
    refresh_state_now(&ui);
    settle_output();
    for (int i = 0; i < 2; i++) {
        GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(views[i]));
        assert_at_bottom(adj);
        gtk_adjustment_set_value(adj, 100);
    }
    g_string_prepend(text, "Updated state\n");
    state_text = text->str;
    ui_log(&ui, "New entry while reading history");
    refresh_state_now(&ui);
    settle_output();
    for (int i = 0; i < 2; i++) {
        GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(views[i]));
        g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(adj), 100, 1.0);
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) -
                                      gtk_adjustment_get_page_size(adj));
    }
    g_string_append(text, "Latest state\n");
    state_text = text->str;
    ui_log(&ui, "Latest log entry");
    refresh_state_now(&ui);
    settle_output();
    for (int i = 0; i < 2; i++)
        assert_at_bottom(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(views[i])));
    /* Crossing the retention limit must keep the newest entry and tail it. */
    for (int i = 0; i < 20; i++) ui_log(&ui, "%s", text->str);
    ui_log(&ui, "Newest retained entry");
    settle_output();
    assert_at_bottom(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(ui.log_view)));
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui.log_view));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    char *retained = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    g_assert_nonnull(strstr(retained, "Newest retained entry"));
    g_assert_cmpint(gtk_text_buffer_get_char_count(buf), <=, 20000);
    g_free(retained);
    state_text = "";
    g_string_free(text, TRUE);
    gtk_widget_destroy(window);
    settle_output();
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    test_output_scrolling();
    LagProfile profile;
    UI ui = {.profile = &profile};
    memset(&profile, 0, sizeof profile);
    ui.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(ui.window), box);
    build_tree(&ui, box);
    build_form(&ui, box);
    ui.log_view = gtk_text_view_new();
    ui.state_view = gtk_text_view_new();
    gtk_box_pack_start(GTK_BOX(box), ui.log_view, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.state_view, FALSE, FALSE, 0);
    guint timer = g_timeout_add(10, dismiss_errors, NULL);
    alarm(15);

    reset(&ui);
    profile.rules[4].active = TRUE; /* unrelated interface must stay untouched */
    select_row(&ui, 0);
    g_assert_true(gtk_widget_get_sensitive(ui.edit_button));
    select_row(&ui, 2);
    select_row(&ui, 3);
    g_assert_false(gtk_widget_get_sensitive(ui.edit_button));
    on_start_clicked(NULL, &ui);
    g_assert_cmpint(calls[0], ==, 1);
    g_assert_cmpint(calls[1], ==, 1);
    g_assert_cmpint(calls[2], ==, 0);
    g_assert_true(applied[0].rules[0].active && applied[0].rules[2].active);
    g_assert_false(profile.rules[1].active);
    g_assert_true(profile.rules[4].active);
    gboolean selected[LAG_MAX_RULES];
    g_assert_cmpint(ui_selected_rules(&ui, selected), ==, 3);
    g_assert_true(selected[0] && selected[2] && selected[3]);
    on_stop_clicked(NULL, &ui);
    g_assert_cmpint(calls[0], ==, 2);
    g_assert_cmpint(calls[1], ==, 2);
    g_assert_false(profile.rules[0].active || profile.rules[2].active || profile.rules[3].active);
    g_assert_true(profile.rules[4].active);
    on_stop_clicked(NULL, &ui);
    g_assert_cmpint(calls[0], ==, 2); /* already stopped is a no-op */

    reset(&ui);
    profile.rules[0].active = profile.rules[1].active = profile.rules[3].active = TRUE;
    select_row(&ui, 0);
    select_row(&ui, 2); /* inactive deletion needs no extra apply */
    select_row(&ui, 3);
    on_delete_clicked(NULL, &ui);
    g_assert_cmpint(calls[0], ==, 1);
    g_assert_cmpint(calls[1], ==, 1);
    g_assert_true(applied[0].rules[1].active);
    g_assert_cmpint(profile.count, ==, 3);
    g_assert_cmpstr(profile.rules[0].dst_port, ==, "1001");
    g_assert_cmpstr(profile.rules[1].dst_port, ==, "1004");
    g_assert_cmpstr(profile.rules[2].dst_port, ==, "1005");
    g_assert_cmpint(ui_selected_rules(&ui, selected), ==, 0);

    reset(&ui);
    profile.rules[0].active = profile.rules[3].active = TRUE;
    fail_iface = "test1";
    select_row(&ui, 0);
    select_row(&ui, 3);
    on_delete_clicked(NULL, &ui);
    g_assert_cmpint(profile.count, ==, 5);
    g_assert_cmpstr(profile.rules[2].dst_port, ==, "1003");
    g_assert_true(profile.rules[2].active); /* failed stop retained for retry */
    g_assert_cmpint(ui_selected_rules(&ui, selected), ==, 1);
    g_assert_true(selected[2]);

    reset(&ui);
    select_row(&ui, 2);
    select_row(&ui, 3);
    GtkTreePath *path = gtk_tree_path_new_from_indices(0, -1);
    on_tree_row_activated(GTK_TREE_VIEW(ui.tree), path, NULL, &ui);
    gtk_tree_path_free(path);
    g_assert_true(profile.rules[0].active);
    g_assert_false(profile.rules[2].active || profile.rules[3].active);
    g_assert_cmpint(ui_selected_rules(&ui, selected), ==, 2);

    reset(&ui);
    profile.rules[1].active = TRUE;
    select_row(&ui, 0);
    select_row(&ui, 3);
    fail_iface = "lo";
    fail_once = TRUE;
    on_start_clicked(NULL, &ui);
    g_assert_false(profile.rules[0].active);
    g_assert_true(profile.rules[1].active); /* existing rule restored */
    g_assert_true(profile.rules[3].active); /* other interface succeeded */
    g_assert_cmpint(calls[0], ==, 2); /* failed apply plus rollback */
    g_assert_cmpint(calls[1], ==, 1);
    g_assert_false(applied[0].rules[0].active);
    g_assert_true(applied[0].rules[1].active);
    g_assert_cmpint(ui_selected_rules(&ui, selected), ==, 2);

    reset(&ui);
    gtk_tree_selection_select_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(ui.tree)));
    g_assert_cmpint(ui_selected_rules(&ui, selected), ==, 6);
    on_start_clicked(NULL, &ui);
    for (int i = 0; i < 4; i++) g_assert_cmpint(calls[i], ==, 1);
    on_stop_all_clicked(NULL, &ui);
    for (int i = 0; i < 4; i++) g_assert_cmpint(calls[i], ==, 2);
    for (int i = 0; i < 6; i++) g_assert_false(profile.rules[i].active);
    on_delete_clicked(NULL, &ui);
    g_assert_cmpint(profile.count, ==, 0);
    for (int i = 0; i < 4; i++) g_assert_cmpint(calls[i], ==, 2);

    g_source_remove(timer);
    gtk_widget_destroy(ui.window);
    g_object_unref(ui.store);
    g_print("Multi-selection: start/stop/delete, batching, failure retention and selection passed.\n");
    return 0;
}
