#ifndef WIDGET_DEMO_H
#define WIDGET_DEMO_H

/* Widget Demo — exercises every widget in gui/widgets/widgets.h: label,
 * button, checkbox, slider, progress bar, textbox, vertical scrollbar,
 * dropdown, colour picker, and the shape primitives (rect/line/circle).
 * Doubles as a living usage reference — see widgets.h's header comment
 * and this app's source for the full pattern (including the documented
 * Esc-vs-textbox-focus interaction). Registered with appman_register()
 * in kernel/appman/appman.c. */
void app_widget_demo(void);

#endif /* WIDGET_DEMO_H */
