/* kernel/keyboard.c — PS/2 keyboard driver (IRQ1, scancode set 1)
 *
 * FIX v1.1 — extended key support:
 *   0xE0-prefixed extended keys (arrows, Home, End, PgUp/Dn, Ins, Del)
 *   are now decoded and emitted as KB_KEY_* constants from keyboard.h.
 *   Arrow codes are pushed to the ring (>= 0x80 space) and consumed by
 *   the desktop for cursor movement and Alt+Tab.
 *
 * FIX v1.1 — ring buffer:
 *   Size bumped to 128 (power of two); wrap uses bitwise AND (no overflow).
 *
 * Reference: https://wiki.osdev.org/PS/2_Keyboard#Scan_Code_Sets
 */
#include "../../types.h"
#include "keyboard.h"
#include "input_router.h"
#include "../../arch/x86_64/irq.h"
#include "../../arch/x86_64/pic.h"
#include "../../log.h"
#include "../../sched/sched.h"

#define KB_DATA   0x60
#define KB_STATUS 0x64
#define KB_BUF    128  /* must be a power of two */

static inline uint8_t inb(uint16_t port) {
    uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

/* ---- Scancode set 1 -> ASCII ------------------------------------------- */
static const char sc_normal[128] = {
    0,    0x1B, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t', 'q',  'w','e','r','t','y','u','i','o','p','[',']','\n',
    0,    'a',  's','d','f','g','h','j','k','l',';','\'','`',
    0,    '\\', 'z','x','c','v','b','n','m',',','.','/', 0,
    '*',  0,    ' ',
    /* 0x3A-0x7F: function keys, numpad, reserved – no ASCII */
    [0x3A]=0,[0x3B]=0,[0x3C]=0,[0x3D]=0,[0x3E]=0,[0x3F]=0,[0x40]=0,
    [0x41]=0,[0x42]=0,[0x43]=0,[0x44]=0,[0x45]=0,[0x46]=0,[0x47]=0,
    [0x48]=0,[0x49]=0,[0x4A]=0,[0x4B]=0,[0x4C]=0,[0x4D]=0,[0x4E]=0,
    [0x4F]=0,[0x50]=0,[0x51]=0,[0x52]=0,[0x53]=0,
};

static const char sc_shifted[128] = {
    0,    0x1B, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t', 'Q',  'W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,    'A',  'S','D','F','G','H','J','K','L',':','"', '~',
    0,    '|',  'Z','X','C','V','B','N','M','<','>','?', 0,
    '*',  0,    ' ',
};

/* ---- IRQ counter (read by irq_watchdog_task in init.c) ----------------- */
volatile uint32_t g_irq1_count = 0;

/* ---- Ring buffer -------------------------------------------------------- */
static uint8_t          ring[KB_BUF];
static volatile uint8_t r_head, r_tail;

/* Modifier and sequence state */
static int shifted;
static int ctrl_dn;
static int alt_dn;
static int ext_seq;   /* 1 when last byte was 0xE0 */

/* Per-key pressed state, indexed by KB_KEY_* */
static uint8_t key_state[256];

static void ring_push(uint8_t c) {
    uint8_t next = (uint8_t)((r_tail + 1u) & (KB_BUF - 1u));
    if (next != r_head) { ring[r_tail] = c; r_tail = next; }
    /* FIX 1.11/2.1: also route to the per-task focused queue */
    input_router_push((char)c);
}

/* Extended scancode -> KB_KEY_* */
static uint8_t ext_to_keycode(uint8_t sc) {
    switch (sc) {
        case 0x48: return KB_KEY_UP;
        case 0x50: return KB_KEY_DOWN;
        case 0x4B: return KB_KEY_LEFT;
        case 0x4D: return KB_KEY_RIGHT;
        case 0x47: return KB_KEY_HOME;
        case 0x4F: return KB_KEY_END;
        case 0x49: return KB_KEY_PGUP;
        case 0x51: return KB_KEY_PGDN;
        case 0x52: return KB_KEY_INS;
        case 0x53: return KB_KEY_DEL;
        case 0x1C: return KB_KEY_KPENTER;
        case 0x35: return KB_KEY_KPSLASH;
        default:   return 0;
    }
}

/* ---- IRQ1 handler ------------------------------------------------------- */
/* Tracks the irq1 count at the time of the last 0xE0 prefix so we can
 * expire a stale ext_seq if the next byte never arrives (e.g. VM lost
 * focus between the 0xE0 and its follow-up scancode). */
static uint32_t ext_seq_irq_stamp = 0;

static void kb_handler(struct isr_frame *f) {
    (void)f;
    g_irq1_count++;
    uint8_t st = inb(KB_STATUS);
    if (!(st & 0x01)) return;   /* output buffer empty — nothing to read */
    if (st & 0x20)    return;   /* aux bit set — this byte belongs to mouse, not keyboard.
                                 * When the VM regains focus the controller can deliver
                                 * buffered mouse bytes on IRQ1; discard them here so they
                                 * don't corrupt the scancode stream. */
    uint8_t sc = inb(KB_DATA);
    /* NOTE: do NOT call kinfo/klog here — logging from IRQ context is unsafe */

    /* Extended-key prefix: next scancode is in extended set.
     * Record the IRQ count so a stale ext_seq can time out if the VM loses
     * focus between the 0xE0 and its follow-on byte. */
    if (sc == 0xE0) { ext_seq = 1; ext_seq_irq_stamp = g_irq1_count; return; }

    /* Handle extended key (arrow keys, Home/End, etc.) */
    if (ext_seq) {
        /* Expire: if more than 50 IRQ1 fires elapsed since the 0xE0 prefix
         * and this byte doesn't look like a valid follow-on (high bit set
         * but not a normal release), the 0xE0 was stranded.  Reset and
         * re-process this byte as a fresh scancode. */
        if ((g_irq1_count - ext_seq_irq_stamp) > 50) {
            ext_seq = 0;
            /* fall through to normal scancode processing below */
        } else {
            ext_seq = 0;
            int release = (sc & 0x80) ? 1 : 0;
            uint8_t base = sc & 0x7F;
            uint8_t kc   = ext_to_keycode(base);
            if (kc) {
                key_state[kc] = release ? 0 : 1;
                if (!release) ring_push(kc);  /* push special KB_KEY_* code */
            }
            /* Alt+Tab detection for task switcher */
            if (!release && base == 0x0F && alt_dn) {
                ring_push(KB_KEY_ALTTAB);
            }
            return;
        }
    }

    /* Standard modifier tracking */
    if (sc == 0x2A || sc == 0x36) { shifted = 1; key_state[KB_KEY_SHIFT] = 1; return; }
    if (sc == 0xAA || sc == 0xB6) { shifted = 0; key_state[KB_KEY_SHIFT] = 0; return; }
    if (sc == 0x1D) { ctrl_dn = 1; key_state[KB_KEY_CTRL] = 1; return; }
    if (sc == 0x9D) { ctrl_dn = 0; key_state[KB_KEY_CTRL] = 0; return; }
    if (sc == 0x38) { alt_dn  = 1; key_state[KB_KEY_ALT]  = 1; return; }
    if (sc == 0xB8) { alt_dn  = 0; key_state[KB_KEY_ALT]  = 0; return; }
    if (sc == 0x3A) { return; } /* CapsLock – not implemented yet */

    if (sc & 0x80) return;   /* key-release codes for non-modifier keys */
    if (sc >= 128)  return;

    /* Alt+Tab: emit special keycode when Tab is pressed with Alt held */
    if (sc == 0x0F && alt_dn) {
        ring_push(KB_KEY_ALTTAB);
        return;
    }

    const char *map = shifted ? sc_shifted : sc_normal;
    char c = map[sc];
    if (!c) return;

    /* Ctrl+letter -> control code (^A=1 .. ^Z=26) */
    if (ctrl_dn) {
        if      (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
    }

    ring_push((uint8_t)c);
}

/* ---- Public API --------------------------------------------------------- */

void keyboard_init(void) {
    r_head = r_tail = 0;
    shifted = ctrl_dn = alt_dn = ext_seq = 0;
    for (int i = 0; i < 256; i++) key_state[i] = 0;

    /* Drain stale bytes in PS/2 output buffer */
    for (int i = 0; i < 16 && (inb(KB_STATUS) & 0x01); i++)
        inb(KB_DATA);

    /* Enable first PS/2 port (keyboard) — wait for controller ready first */
    for (int t = 100000; t-- && (inb(KB_STATUS) & 0x02););
    outb(KB_STATUS, 0xAE);

    irq_register(33, kb_handler);
    pic_unmask(1);
    kinfo("KEYBOARD: PS/2 IRQ1 registered, extended keys active\n");
}

int keyboard_getchar(void) {
    if (r_head == r_tail) return 0;
    uint8_t c = ring[r_head];
    r_head = (uint8_t)((r_head + 1u) & (KB_BUF - 1u));
    return (int)(uint8_t)c;   /* widen without sign-extension */
}

int keyboard_read(void) {
    int c;
    /* Use sched_yield() instead of bare "sti; hlt" so the scheduler
     * can run other tasks while we wait.  "sti; hlt" wakes on every
     * interrupt (including IRQ12 mouse), and under heavy mouse traffic
     * the CPU re-enters this loop so fast that the PIC's IRR can lose
     * the IRQ1 pending edge, causing the keyboard to go silent. */
    while (!(c = keyboard_getchar()))
        sched_yield();
    return c;
}

int keyboard_key_down(uint8_t keycode) { return (int)key_state[keycode]; }
int keyboard_shift(void) { return shifted; }
int keyboard_ctrl(void)  { return ctrl_dn; }
int keyboard_alt(void)   { return alt_dn;  }

/* keyboard_reinit — called after mouse_init() to guarantee PS/2 controller
 * has kbd IRQ1 enabled.  mouse_init() reads and rewrites the PS/2 config
 * byte and may leave bit 0 (kbd IRQ1 enable) or bit 4 (kbd clock disable)
 * in an undesired state depending on what the BIOS left behind.
 * This sequence mirrors the fix recommended by OSDev wiki PS/2 article:
 * https://wiki.osdev.org/PS/2_Keyboard#Initialisation
 */
static inline void ps2_wait_w(void) { int t=100000; while(--t&&(inb(0x64)&0x02)); }
static inline void ps2_wait_r(void) { int t=100000; while(--t&&!(inb(0x64)&0x01)); }

void keyboard_reinit(void) {
    /* 1. Enable first PS/2 port via controller command 0xAE */
    ps2_wait_w();  outb(0x64, 0xAE);

    /* 2. Read current config byte (command 0x20) */
    ps2_wait_w();  outb(0x64, 0x20);
    ps2_wait_r();
    uint8_t cfg = inb(0x60);

    /* 3. Force: kbd IRQ1 enabled (bit0=1), kbd translation on (bit6=1),
     *           kbd clock enabled (bit4=0) */
    cfg |=  0x41;   /* bit0=IRQ1 enable, bit6=translation on */
    cfg &= ~0x10;   /* bit4=kbd clock disable → clear to enable */

    /* 4. Write config back (command 0x60) */
    ps2_wait_w();  outb(0x64, 0x60);
    ps2_wait_w();  outb(0x60, cfg);

    /* 5. Readback: verify bit6 stuck */
    ps2_wait_w();  outb(0x64, 0x20);
    ps2_wait_r();
    uint8_t cfg_v = inb(0x60);
    if (!(cfg_v & 0x40))
        kwarn("KEYBOARD: reinit — bit6 (translation) did not stick "
              "(cfg=0x%02x) — scancodes may be wrong\n", (unsigned)cfg_v);

    /* 6. PIC: ensure IRQ1 is unmasked */
    pic_unmask(1);

    kinfo("KEYBOARD: reinit complete (PS/2 cfg=0x%02x, IRQ1 unmasked)\n",
          (unsigned)cfg);
}
