/* SPDX-License-Identifier: MIT */
#ifndef LAG_PROFILE_H
#define LAG_PROFILE_H

#include "lag.h"

void  profile_clear(LagProfile *p);
char *profile_default_path(void);   /* newly allocated: ~/.config/testlag/profile.json */
int   profile_save(const LagProfile *p, const char *path, char *err, size_t errlen);
/* Load a profile. Returns 0 on success, -1 with err filled on failure.
 * Rules whose fields do not validate are dropped rather than loaded; on
 * success err is either empty or a warning naming the dropped rules. */
int   profile_load(LagProfile *p, const char *path, char *err, size_t errlen);

#endif /* LAG_PROFILE_H */
