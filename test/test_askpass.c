/* SPDX-License-Identifier: MIT */
/* Run with a test display, e.g. GDK_BACKEND=broadway BROADWAY_DISPLAY=:77. */
#include <gtk/gtk.h>
#include <sys/wait.h>
#include <unistd.h>
#include "askpass.h"

static GtkWidget *find_entry(GtkWidget *widget) {
    if (GTK_IS_ENTRY(widget)) return widget;
    if (!GTK_IS_CONTAINER(widget)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget *found = NULL;
    for (GList *it = children; it && !found; it = it->next)
        found = find_entry(it->data);
    g_list_free(children);
    return found;
}

static gboolean respond(gpointer data) {
    int mode = GPOINTER_TO_INT(data);
    GList *windows = gtk_window_list_toplevels();
    for (GList *it = windows; it; it = it->next) {
        if (!GTK_IS_DIALOG(it->data)) continue;
        GtkWidget *entry = find_entry(it->data);
        g_assert_nonnull(entry);
        g_assert_false(gtk_entry_get_visibility(GTK_ENTRY(entry)));
        gtk_entry_set_text(GTK_ENTRY(entry), mode == 3 ? "" : "fake-test-password");
        gtk_dialog_response(GTK_DIALOG(it->data),
            mode == 1 ? GTK_RESPONSE_CANCEL :
            mode == 2 ? GTK_RESPONSE_DELETE_EVENT : GTK_RESPONSE_OK);
        g_list_free(windows);
        return G_SOURCE_REMOVE;
    }
    g_list_free(windows);
    return G_SOURCE_CONTINUE;
}

int main(void) {
    for (int mode = 0; mode < 4; mode++) {
        int fds[2];
        g_assert_cmpint(pipe(fds), ==, 0);
        pid_t pid = fork();
        g_assert_cmpint(pid, >=, 0);
        if (pid == 0) {
            close(fds[0]);
            g_assert_cmpint(dup2(fds[1], STDOUT_FILENO), >=, 0);
            close(fds[1]);
            alarm(10);
            g_timeout_add(20, respond, GINT_TO_POINTER(mode));
            _exit(askpass_run("[sudo] password for test user:"));
        }
        close(fds[1]);
        GString *output = g_string_new(NULL);
        char buf[128];
        ssize_t n;
        while ((n = read(fds[0], buf, sizeof buf)) > 0)
            g_string_append_len(output, buf, n);
        close(fds[0]);
        int status;
        g_assert_cmpint(waitpid(pid, &status, 0), ==, pid);
        g_assert_true(WIFEXITED(status));
        g_assert_cmpint(WEXITSTATUS(status), ==, mode == 0 ? 0 : 1);
        g_assert_cmpstr(output->str, ==, mode == 0 ? "fake-test-password\n" : "");
        g_string_free(output, TRUE);
    }
    g_print("Askpass: submission, cancellation, close and empty input passed.\n");
    return 0;
}
