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
#include "../../sched/sched.h"   /* sched_current_id(), sched_yield() */

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

/* ---- Ring buffer — kept for early-boot fallback only -------------------
 * NOTE: ring_push() no longer writes to ring[]. All input is routed
 * exclusively through input_router so keyboard_getchar() only reads from
 * one place per task. Writing to both caused every key to appear twice:
 *   1. input_router_getchar() consumed it → returned
 *   2. ring[] still had it → returned again on next call (fallback path)
 * The ring[] variables are kept but unused to avoid touching the rest of
 * the struct/header. */
static uint8_t          ring[KB_BUF];
static volatile uint8_t r_head, r_tail;

/* Modifier and sequence state */
static int shifted;
static int ctrl_dn;
static int alt_dn;
static int super_dn;  /* Super / Windows key held */
static int caps_lock;  /* 1 = Caps Lock active (toggled on press) */
static int ext_seq;    /* 1 when last byte was 0xE0 */

/* Per-key pressed state, indexed by KB_KEY_* */
static uint8_t key_state[256];

static void ring_push(uint8_t c) {
    /* FIX: write input_router first (primary path), then ring[] for sync.
     * keyboard_getchar() reads ONLY from input_router now, so ring[] is
     * kept in sync only for potential legacy/debug reads — NOT consumed
     * by keyboard_getchar(), which eliminates the double-key delivery. */
    input_router_push((char)c);
    uint8_t next = (uint8_t)((r_tail + 1u) & (KB_BUF - 1u));
    if (next != r_head) { ring[r_tail] = c; r_tail = next; }
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
        case 0x5B: return KB_KEY_SUPER;  /* Left Windows / Super key */
        case 0x5C: return KB_KEY_SUPER;  /* Right Windows / Super key */
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
                if (!release) {
                    if (kc == KB_KEY_SUPER) {
                        super_dn = 1;
                        key_state[KB_KEY_SUPER_M] = 1;
                        /* Don't push to ring — Super is a pure modifier */
                    } else {
                        ring_push(kc);
                    }
                } else if (kc == KB_KEY_SUPER) {
                    super_dn = 0;
                    key_state[KB_KEY_SUPER_M] = 0;
                }
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
    /* Caps Lock — toggle on press (0x3A), ignore release (0xBA) */
    if (sc == 0x3A) { caps_lock = !caps_lock; key_state[KB_KEY_CAPS] = (uint8_t)caps_lock; return; }
    if (sc == 0xBA) { return; } /* Caps Lock release — already handled on press */

    /* BUG FIX (workspace-switch keys never worked — dead code): F1-F12 are
     * plain, non-extended (no 0xE0 prefix) scancodes 0x3B-0x44 (F1-F10)
     * and 0x57-0x58 (F11-F12) on a standard PS/2 keyboard, but sc_normal/
     * sc_shifted map that whole range to 0 (unmapped) since they have no
     * ASCII representation — so they fell straight into "if (!c) return;"
     * further below and were silently dropped before ever reaching
     * ring_push(). KB_KEY_F1..F12 constants already existed in
     * keyboard.h and desktop.c already had F1-F4 workspace-switch
     * handling written against them — it just never fired, since these
     * keycodes were never actually produced. Handled the same way the
     * extended-key table handles arrows/Home/End/etc: press pushes the
     * keycode, release just updates key_state. Checked BEFORE the
     * general "sc & 0x80 -> release, bail" line below so releases are
     * correctly cleared in key_state instead of leaving it stuck "held"
     * forever after the first press. */
    {
        uint8_t base = sc & 0x7F;
        int release = (sc & 0x80) ? 1 : 0;
        static const uint8_t fkey_map[] = {
            [0x3B]=KB_KEY_F1, [0x3C]=KB_KEY_F2, [0x3D]=KB_KEY_F3, [0x3E]=KB_KEY_F4,
            [0x3F]=KB_KEY_F5, [0x40]=KB_KEY_F6, [0x41]=KB_KEY_F7, [0x42]=KB_KEY_F8,
            [0x43]=KB_KEY_F9, [0x44]=KB_KEY_F10,
        };
        uint8_t kc = 0;
        if (base < sizeof(fkey_map)) kc = fkey_map[base];
        else if (base == 0x57) kc = KB_KEY_F11;
        else if (base == 0x58) kc = KB_KEY_F12;
        if (kc) {
            key_state[kc] = release ? 0 : 1;
            if (!release) {
                /* BUG FIX (workspace switching blocked while an app has
                 * focus): F1-F4 are DracolaxOS's workspace-switch keys
                 * (see desktop.c and ws_switcher.c). Like Super+letter
                 * above, these are a system-level shortcut the user must
                 * always be able to trigger — not something that should
                 * depend on which app window currently owns keyboard
                 * focus. Routed the same way: always straight to the
                 * desktop task, bypassing the normal per-focus queue.
                 * F5-F12 aren't bound to anything yet, so they keep the
                 * normal focus-gated routing (ring_push) for whichever
                 * app/future feature ends up using them. */
                if (kc == KB_KEY_F1 || kc == KB_KEY_F2 || kc == KB_KEY_F3 || kc == KB_KEY_F4)
                    input_router_push_to(g_desktop_task, (char)kc);
                else
                    ring_push(kc);
            }
            return;
        }
    }

    if (sc & 0x80) return;   /* key-release codes for non-modifier keys */
    if (sc >= 128)  return;

    /* Alt+Tab: emit special keycode when Tab is pressed with Alt held */
    /* BUG FIX: Alt+Tab (now the open-apps dock toggle — see dock.c) is a
     * system-level shortcut just like F1-F4/Super+letter, so it must
     * always reach the desktop task regardless of which app currently
     * has keyboard focus. This constant was already defined
     * ("synthesised: Alt+Tab for task switcher" per keyboard.h) but
     * nothing ever consumed it — it was entirely unused dead code until
     * now. */
    if (sc == 0x0F && alt_dn) {
        input_router_push_to(g_desktop_task, (char)KB_KEY_ALTTAB);
        return;
    }

    const char *map = shifted ? sc_shifted : sc_normal;
    char c = map[sc];
    if (!c) return;

    /* Apply Caps Lock to letter keys only.
     * Caps Lock inverts the shift state for a-z / A-Z:
     *   no-shift + caps → uppercase
     *   shift    + caps → lowercase (same as shift cancels caps) */
    if (caps_lock && !ctrl_dn) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);   /* → uppercase */
        else if (c >= 'A' && c <= 'Z') c = (char)(c + 32); /* → lowercase */
    }

    /* Ctrl+letter -> control code (^A=1 .. ^Z=26) */
    if (ctrl_dn) {
        if      (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
    }

    /* Super+letter -> 0xC0 | (letter_index 0-25).
     * ALWAYS sent to the desktop task (registered via input_router_set_desktop_task)
     * so Super+S/W/B/etc work even when an app window has keyboard focus. */
    if (super_dn && !ctrl_dn) {
        char lc = c;
        if (lc >= 'A' && lc <= 'Z') lc = (char)(lc + 32);
        if (lc >= 'a' && lc <= 'z') {
            input_router_push_to(g_desktop_task, (char)(0xC0u | (uint8_t)(lc - 'a')));
            return;
        }
    }

    /* Ctrl+Super+letter -> 0xE0 | (letter_index 0-25). Same always-
     * reaches-the-desktop bypass as plain Super+letter above, in its own
     * encoded range so the two combos are distinguishable on the
     * receiving end. Currently only Ctrl+Super+D is bound to anything
     * (GUI debug mode toggle — see desktop.c), but the whole range is
     * wired up the same way Super+letter's is, for any future combo.
     *
     * NOTE: by this point, if ctrl_dn is set, `c` has ALREADY been
     * transformed by the "Ctrl+letter -> control code" block above (e.g.
     * Ctrl+D -> 4), so it's no longer a letter — recover it by reversing
     * that transform (control code 1-26 -> 'a'-'z') instead of checking
     * for a letter that isn't there anymore. */
    if (super_dn && ctrl_dn) {
        char lc = (c >= 1 && c <= 26) ? (char)('a' + c - 1) : 0;
        if (lc >= 'a' && lc <= 'z') {
            input_router_push_to(g_desktop_task, (char)(0xE0u | (uint8_t)(lc - 'a')));
            return;
        }
    }

    ring_push((uint8_t)c);
}

void keyboard_init(void) {
    r_head = r_tail = 0;
    shifted = ctrl_dn = alt_dn = super_dn = ext_seq = caps_lock = 0;
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
    /* Read ONLY from this task's per-task input_router queue.
     * The old dual-path (input_router first, then ring[] fallback) caused
     * every key to be delivered twice: input_router consumed it and returned,
     * then the next call fell through to ring[] which still had the same char.
     * Removing the ring[] fallback eliminates all double-key events. */
    int c = input_router_getchar(sched_current_id());
    return (c >= 0) ? c : 0;
}

int keyboard_read(void) {
    int c;
    while (!(c = keyboard_getchar()))
        sched_yield();
    return c;
}

int keyboard_key_down(uint8_t keycode) { return (int)key_state[keycode]; }
int keyboard_shift(void)     { return shifted; }
int keyboard_ctrl(void)      { return ctrl_dn; }
int keyboard_alt(void)       { return alt_dn;  }
int keyboard_super(void)     { return super_dn; }
int keyboard_caps_lock(void) { return caps_lock; }

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
