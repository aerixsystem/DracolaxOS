/* kernel/vmmouse.h — VMware/QEMU absolute mouse (vmmouse) backdoor
 *
 * When running under QEMU (-device vmware-svga or plain -vga std with
 * the vmmouse device) or VMware, the guest can communicate through the
 * VMware backdoor I/O port (0x5658) to receive ABSOLUTE mouse coordinates
 * instead of relative PS/2 deltas.
 *
 * Effect: the hypervisor no longer requires the user to "capture" the mouse.
 * The annoying "guest OS does not support mouse pointer integration" message
 * goes away because the guest OS now actively reads absolute coords.
 *
 * Reference: https://wiki.osdev.org/VMware_tools
 *            https://github.com/vmware/open-vm-tools
 */
#ifndef VMMOUSE_H
#define VMMOUSE_H
#include "../../types.h"

/** Try to detect and initialise the VMware/QEMU vmmouse backdoor.
 *  Returns 1 if available (absolute mode active), 0 if not (PS/2 relative). */
int vmmouse_init(void);

/** Returns 1 if vmmouse is active. */
int vmmouse_active(void);

/** Read absolute mouse position from hypervisor and update the PS/2 mouse
 *  state (mx, my, buttons).  Call once per compositor frame.
 *  Does nothing if vmmouse is not active. */
void vmmouse_poll(void);

#endif /* VMMOUSE_H */
