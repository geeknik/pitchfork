/* See COPYING.txt for license details. */

/* m1_ui_focus.c — display-focus lease registry. See m1_ui_focus.h for the
 * policy; this file is intentionally trivial and allocation-free so it can
 * never fail, block, or leak. */

#include "m1_ui_focus.h"

static void *s_owner;

void m1_ui_focus_grant(void *owner)
{
    (void)m1_ui_focus_try_grant(owner);
}

bool m1_ui_focus_try_grant(void *owner)
{
    if (owner == 0)
        return false;            /* NULL must never be a lease token: it is
                                     indistinguishable from "free" */
    if (s_owner != 0)
        return false;            /* already leased — do not silently steal */
    s_owner = owner;
    return true;
}

bool m1_ui_focus_revoke(void *owner)
{
    if (s_owner == 0 || s_owner != owner)
        return false;            /* free, or held by someone else */
    s_owner = 0;
    return true;
}

bool m1_ui_focus_is_free(void)          { return s_owner == 0; }
bool m1_ui_focus_held_by(const void *w) { return s_owner != 0 && s_owner == w; }

bool m1_ui_focus_allowed(const void *who)
{
    return s_owner == 0 || s_owner == who;
}
