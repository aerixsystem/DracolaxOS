/* kernel/vmmouse.c — VMware/QEMU absolute mouse backdoor driver
 *
 * Protocol summary (VMware backdoor, port 0x5658):
 *   All communication uses the IN/OUT instructions with a magic cookie.
 *
 *   EAX = VMWARE_MAGIC  (0x564D5868)
 *   EBX = command-specific parameter
 *   ECX = command number
 *   EDX = port | (version << 16)    (port = 0x5658)
 *
 *   After IN: EAX/EBX/ECX/EDX carry the response.
 *
 * Commands used:
 *   GETVERSION  (0x0A) — sanity check: EAX must still be VMWARE_MAGIC
 *   ABSPTR_CMD  (0x27) — sub-commands for absolute pointer:
 *       sub 0x45414552 = ABSPTR_DATA   (get queued packet count)
 *       sub 0x53454552 = ABSPTR_STATUS (read one absolute packet)
 *       sub 0x00000000 = ABSPTR_OLD    (disable absolute mode)
 *
 * Absolute packet fields (EBX after ABSPTR_DATA):
 *   bits 31-16: Y  (0–0xFFFF)
 *   bits 15-0 : X  (0–0xFFFF)
 *   ECX bits 2-0 : buttons (same mask as PS/2: bit0=L bit1=R bit2=M)
 *
 * If the backdoor is absent (real hardware / non-QEMU), the IN will
 * trigger a #GP that we catch by checking EAX ≠ VMWARE_MAGIC on return.
 * We use a lightweight inline-asm approach that is safe: if the port
 * does not exist the CPU delivers #GP which is caught by our IDT as a
 * kernel fault.  To avoid that we use CPUID vendor detection first.
 */
#include "../../types.h"
#include "vmmouse.h"
#include "../vga/fb.h"
#include "../../log.h"
#include "mouse.h"   /* mouse_set_absolute — declared below, defined in mouse.c */

/* ---- VMware backdoor constants ------------------------------------------ */
#define VMWARE_MAGIC        0x564D5868u
#define VMWARE_PORT         0x5658u
#define VMWARE_CMD_GETVER   0x0Au
#define VMWARE_CMD_ABSPTR   0x27u

#define ABSPTR_ENABLE   0x45414552u   /* sub-command: request absolute data */
#define ABSPTR_RELATIVE 0x46524552u   /* sub-command: back to relative      */
#define ABSPTR_DATA     0x53454552u   /* sub-command: read one abs packet    */

/* ---- state --------------------------------------------------------------- */
static int vm_active = 0;

/* ---- port I/O via VMware backdoor --------------------------------------- */
typedef struct {
    uint32_t eax, ebx, ecx, edx;
} vmcall_t;

static vmcall_t vmware_call(uint32_t cmd, uint32_t param) {
    vmcall_t r;
    __asm__ volatile (
        "inl %%dx, %%eax\n\t"
        : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
        : "a"(VMWARE_MAGIC),
          "b"(param),
          "c"(cmd),
          "d"(VMWARE_PORT)
    );
    return r;
}

/* ---- CPUID-based hypervisor vendor detection ---------------------------- */
/* Returns 1 if CPUID leaf 0x40000000 returns "VMwareVMware" or "KVMKVMKVM\0\0\0" */
static int detect_hypervisor(void) {
    uint32_t ebx, ecx, edx, eax;
    __asm__ volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0x40000000u)
    );
    /* VMware: "VMwareVMware" — EBX="VMwa" ECX="reVM" EDX="ware" */
    if (ebx == 0x61774D56u && ecx == 0x4D566572u && edx == 0x65726177u)
        return 1;
    /* VirtualBox: "VBoxVBoxVBox" — EBX="VBox" ECX="VBox" EDX="VBox" */
    if (ebx == 0x786F4256u && ecx == 0x786F4256u && edx == 0x786F4256u)
        return 1;
    /* QEMU/KVM: "KVMKVMKVM\0\0\0" */
    if (ebx == 0x4D564B4Bu && ecx == 0x4B4D564Bu && edx == 0x0000004Du)
        return 1;
    /* QEMU without KVM: "TCGTCGTCGTCG" */
    if (ebx == 0x54474354u)
        return 1;
    /* Any other hypervisor that exposes the VMware backdoor port */
    return 0;
}

/* ---- Public API --------------------------------------------------------- */

int vmmouse_init(void) {
    vm_active = 0;

    /* Step 1: confirm we're inside a hypervisor */
    if (!detect_hypervisor()) {
        kinfo("VMMOUSE: no hypervisor detected, staying with PS/2 relative\n");
        return 0;
    }

    /* Step 2: verify VMware backdoor is alive (GETVERSION) */
    vmcall_t v = vmware_call(VMWARE_CMD_GETVER, 0xFFFFFFFFu);
    if (v.eax != VMWARE_MAGIC) {
        kinfo("VMMOUSE: backdoor not responding (eax=0x%x)\n", v.eax);
        return 0;
    }
    kinfo("VMMOUSE: backdoor OK, version=%u\n", (unsigned)v.ebx);

    /* Step 3: request absolute pointer mode */
    vmware_call(VMWARE_CMD_ABSPTR, ABSPTR_ENABLE);

    vm_active = 1;
    kinfo("VMMOUSE: absolute pointer mode active — no mouse capture needed\n");
    return 1;
}

int vmmouse_active(void) {
    return vm_active;
}

/* Forward-declared setter in mouse.c — lets vmmouse push absolute coords
 * without mouse.c needing to know about vmmouse internals. */
extern void mouse_set_pos(int x, int y);
extern void mouse_set_buttons(uint8_t b);

void vmmouse_poll(void) {
    if (!vm_active || !fb.available) return;

    /* Read pending packet count */
    vmcall_t status = vmware_call(VMWARE_CMD_ABSPTR, ABSPTR_DATA);
    uint32_t count = status.ebx;
    if (count == 0) return;

    /* Read the absolute position packet */
    vmcall_t pkt = vmware_call(VMWARE_CMD_ABSPTR, ABSPTR_DATA);

    /* EBX bits[15:0]  = X in range 0–0xFFFF
     * EBX bits[31:16] = Y in range 0–0xFFFF
     * ECX bits[2:0]   = buttons (L=1 R=2 M=4) */
    uint32_t abs_x = pkt.ebx & 0xFFFFu;
    uint32_t abs_y = (pkt.ebx >> 16) & 0xFFFFu;
    uint8_t  btns  = (uint8_t)(pkt.ecx & 0x07u);

    /* Scale 0–0xFFFF to screen pixels */
    int sx = (int)(abs_x * (fb.width  - 1) / 0xFFFFu);
    int sy = (int)(abs_y * (fb.height - 1) / 0xFFFFu);

    mouse_set_pos(sx, sy);
    mouse_set_buttons(btns);
}
