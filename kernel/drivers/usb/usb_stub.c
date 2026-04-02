/* drivers/usb/usb_stub.c — USB host controller detection
 *
 * A full XHCI/EHCI driver is a V2 item.  For V1 we probe PCI configuration
 * space for USB host controllers so the boot log reports what hardware is
 * present.  This lets developers know which controller to target for V2.
 *
 * PCI class 0x0C, subclass 0x03 = USB controller.
 *   Programming interface 0x00 = UHCI  (Intel, legacy)
 *   Programming interface 0x10 = OHCI  (OpenHCI, ARM/MIPS embedded)
 *   Programming interface 0x20 = EHCI  (USB 2.0 high-speed)
 *   Programming interface 0x30 = XHCI  (USB 3.x)
 *   Programming interface 0xFE = USB Device (non-host)
 */
#include "../../types.h"
#include "../../log.h"

/* PCI config space access via I/O ports 0xCF8 / 0xCFC */
static inline uint32_t pci_read32(uint8_t bus, uint8_t dev,
                                   uint8_t fn, uint8_t reg) {
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)fn   <<  8)
                  | (reg & 0xFC);
    /* FIX: 0xCF8/0xCFC > 255 so the immediate form of outl/inl is invalid.
     * Use the "Nd" constraint which routes through %%dx for ports > 255.
     * %%w1 forces the 16-bit (%%dx) form required by outl/inl port operand. */
    __asm__ volatile("outl %0, %w1" :: "a"(addr), "Nd"((uint16_t)0xCF8u));
    uint32_t v;
    __asm__ volatile("inl %w1, %0" : "=a"(v) : "Nd"((uint16_t)0xCFCu));
    return v;
}

static const char *usb_type(uint8_t pi) {
    switch (pi) {
    case 0x00: return "UHCI (USB 1.1)";
    case 0x10: return "OHCI (USB 1.1)";
    case 0x20: return "EHCI (USB 2.0)";
    case 0x30: return "XHCI (USB 3.x)";
    case 0xFE: return "USB Device";
    default:   return "USB (unknown)";
    }
}

void usb_init(void) {
    int found = 0;

    /* Scan first 8 buses × 32 devices × 8 functions */
    for (uint8_t bus = 0; bus < 8; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint32_t id = pci_read32(bus, dev, fn, 0x00);
                if (id == 0xFFFFFFFF) continue;   /* slot empty */

                uint32_t cc = pci_read32(bus, dev, fn, 0x08);
                uint8_t  class_code = (uint8_t)(cc >> 24);
                uint8_t  subclass   = (uint8_t)(cc >> 16);
                uint8_t  prog_if    = (uint8_t)(cc >>  8);

                if (class_code == 0x0C && subclass == 0x03) {
                    uint16_t vendor = (uint16_t)(id & 0xFFFF);
                    uint16_t device = (uint16_t)(id >> 16);
                    kinfo("USB: found %s controller — "
                          "PCI %02x:%02x.%x  vid=0x%04x did=0x%04x "
                          "(driver: V2 roadmap)\n",
                          usb_type(prog_if),
                          (unsigned)bus, (unsigned)dev, (unsigned)fn,
                          (unsigned)vendor, (unsigned)device);
                    found++;
                }
            }
        }
    }

    if (!found)
        kinfo("USB: no USB host controller found on PCI bus 0-7\n");
    else
        kinfo("USB: detected %d controller(s) — "
              "XHCI/EHCI driver pending (V2)\n", found);
}
