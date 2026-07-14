#ifndef PACKAGE_MANAGER_H
#define PACKAGE_MANAGER_H

/* Package Manager — STUB
 *
 * This app was removed during the GUI finalization pass (see
 * docs/CHANGELOG.md and docs/developer_notes.md) so it can be rebuilt
 * properly on top of the immediate-mode widget toolkit
 * (gui/widgets/widgets.h) instead of carrying over the old hand-drawn
 * implementation. This is an empty scaffold: the function exists and is
 * safe to call (it exits immediately without creating a window), but it
 * is NOT registered with appman_register() and will not appear in the
 * app launcher/search/dock/desktop until it's reimplemented and
 * re-registered in kernel/appman/appman.c.
 */
void app_pkg_manager(void);

#endif /* PACKAGE_MANAGER_H */
