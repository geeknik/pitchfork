/* See COPYING.txt for license details. */

/*
 * m1_ui_focus.h — display-focus lease registry (the thesis resource-lease
 * model proven on its first, least dangerous resource).
 *
 * One exclusive owner at a time: either FOCUS IS FREE (the kernel's own UI —
 * menu, settings — may draw) or it is held by exactly one task (the running
 * foreground app). Grant refuses when already held (a leaked lease is a bug
 * to log, not to silently overwrite); revoke is idempotent and only succeeds
 * for the current owner, so a stale or racing revoker cannot steal focus
 * from a legitimate holder.
 *
 * Deliberately FreeRTOS-free: owners are opaque tokens (TaskHandle_t on
 * device, plain pointers in host tests), so the whole policy is pure and
 * unit-testable. Callers pass xTaskGetCurrentTaskHandle() themselves.
 */

#ifndef M1_UI_FOCUS_H
#define M1_UI_FOCUS_H

#include <stdbool.h>

void m1_ui_focus_grant(void *owner);          /* takes the lease; refuses if held */
bool m1_ui_focus_try_grant(void *owner);      /* same, but reports success */
bool m1_ui_focus_revoke(void *owner);         /* idempotent; only the owner revokes */
bool m1_ui_focus_is_free(void);
bool m1_ui_focus_held_by(const void *who);
/* May `who` draw right now? True when free, or when `who` is the owner. */
bool m1_ui_focus_allowed(const void *who);

#endif /* M1_UI_FOCUS_H */
