/* SPDX-License-Identifier: MIT */
#ifndef TESTLAG_ASKPASS_H
#define TESTLAG_ASKPASS_H

/* Separate helper process: stdout is the private password pipe to sudo. */
int askpass_run(const char *prompt);

#endif
