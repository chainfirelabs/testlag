/* SPDX-License-Identifier: MIT */
#ifndef LAG_UI_H
#define LAG_UI_H

#include <gtk/gtk.h>
#include "lag.h"

/* Show the main window and run the GTK main loop.
 * The profile is saved back to profile_path on exit. profile_path is copied,
 * not adopted: the caller keeps ownership of the string it passes in. */
void ui_run(LagProfile *profile, const char *profile_path, gboolean use_sudo);

#endif /* LAG_UI_H */
