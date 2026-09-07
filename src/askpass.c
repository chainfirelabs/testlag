/* SPDX-License-Identifier: MIT */
#include "askpass.h"

#include <gtk/gtk.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

int askpass_run(const char *prompt) {
    /* Keep passwords out of core dumps, argv, environment and application logs. */
    struct rlimit limit = {0, 0};
    if (setrlimit(RLIMIT_CORE, &limit) != 0)
        return 1;
    signal(SIGPIPE, SIG_IGN);
    g_set_application_name("TestLag");
    if (!gtk_init_check(NULL, NULL)) {
        g_printerr("TestLag: cannot open the password window (no graphical display).\n");
        return 1;
    }

    GtkWidget *dialog = gtk_dialog_new_with_buttons("TestLag — Authentication",
        NULL, GTK_DIALOG_MODAL, "_Cancel", GTK_RESPONSE_CANCEL,
        "_Authenticate", GTK_RESPONSE_OK, NULL);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 18);
    gtk_box_set_spacing(GTK_BOX(box), 12);
    GtkWidget *info = gtk_label_new("Authentication is required to change network rules.");
    gtk_box_pack_start(GTK_BOX(box), info, FALSE, FALSE, 0);
    /* Show sudo's actual prompt: policy may require a different user's password. */
    GtkWidget *label = gtk_label_new(prompt);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(entry);

    int status = 1;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const char *value = gtk_entry_get_text(GTK_ENTRY(entry));
        /* Askpass is a single-line protocol. Empty input means cancellation. */
        if (*value && !strpbrk(value, "\r\n")) {
            char *password = g_strconcat(value, "\n", NULL);
            size_t len = strlen(password), offset = 0;
            gtk_entry_set_text(GTK_ENTRY(entry), "");
            while (offset < len) {
                ssize_t n = write(STDOUT_FILENO, password + offset, len - offset);
                if (n > 0) offset += (size_t)n;
                else if (n < 0 && errno == EINTR) continue;
                else break;
            }
            status = offset == len ? 0 : 1;
            explicit_bzero(password, len);
            g_free(password);
        }
    }
    gtk_entry_set_text(GTK_ENTRY(entry), "");
    gtk_widget_destroy(dialog);
    return status;
}
