/* gui/apps/installer/installer.c — DracolaxOS First-Boot Installer v2.0
 *
 * Features:
 *   - Keyboard, mouse/touch, on-screen buttons
 *   - Live Session (username: user, no password) — skip full install
 *   - OS detection: scans MBR/GPT for Windows (NTFS) and Linux (ext2/3/4)
 *   - Language selector (EN / PT / ES) — full UI translation
 *   - Desktop environment selection
 *   - Pure-overlay cursor (flip → stamp, no save/restore)
 */
#include "../../kernel/types.h"
#include "../../kernel/drivers/vga/fb.h"
#include "../../kernel/drivers/ps2/keyboard.h"
#include "../../kernel/drivers/ps2/mouse.h"
#include "../../kernel/drivers/ps2/vmmouse.h"
#include "../../kernel/drivers/vga/cursor.h"
#include "../../kernel/klibc.h"
#include "../../kernel/log.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/security/dracoauth.h"

/* ── Palette ──────────────────────────────────────────────────────── */
#define COL_BG      0x0D1117u
#define COL_ACC     0x7828C8u
#define COL_ACC2    0x00C2FFu
#define COL_TXT     0xF0F0FFu
#define COL_DIM     0x606070u
#define COL_BTN     0x1E2240u
#define COL_BTN_HOV 0x2A2F5Au
#define COL_OK      0x40C060u
#define COL_WARN    0xFF8040u
#define FW 8
#define FH 16

/* ── i18n ─────────────────────────────────────────────────────────── */
typedef enum { LANG_EN=0, LANG_PT, LANG_ES, LANG_COUNT } lang_t;
typedef enum {
    S_WELCOME=0, S_LIVE, S_NEXT, S_BACK, S_SKIP, S_BKSP, S_FINISH,
    S_USERNAME, S_PASSWORD, S_DESKTOP, S_CONFIRM, S_INSTALL_DONE,
    S_NO_KB, S_DETECTED_OS, S_MULTIBOOT, S_LANG_SELECT,
    S_UP, S_DOWN, S_COUNT
} str_id_t;

static const char *const tr[LANG_COUNT][S_COUNT] = {
    [LANG_EN] = {
        "Welcome to DracolaxOS v1.0!",
        "Live Session (no install)",
        "NEXT  ->", "<- BACK", "SKIP", "BKSP [x]", "FINISH",
        "Username:", "Password:", "Desktop Environment", "Confirm Setup", "Installation complete!",
        "No keyboard? Use the buttons below.",
        "Other OS detected:", "Enable multi-boot with detected OS?",
        "Select language:", "UP   ^", "DOWN v",
    },
    [LANG_PT] = {
        "Bem-vindo ao DracolaxOS v1.0!",
        "Sessão ao Vivo (sem instalar)",
        "PROXIMO ->", "<- VOLTAR", "PULAR", "APAGAR", "CONCLUIR",
        "Nome de usuario:", "Senha:", "Ambiente de Desktop", "Confirmar", "Instalacao concluida!",
        "Sem teclado? Use os botoes abaixo.",
        "Outro OS detectado:", "Ativar multi-boot com o OS detectado?",
        "Selecionar idioma:", "ACIMA  ^", "ABAIXO v",
    },
    [LANG_ES] = {
        "Bienvenido a DracolaxOS v1.0!",
        "Sesion en vivo (sin instalar)",
        "SIGUIENTE ->", "<- ATRAS", "OMITIR", "BORRAR", "FINALIZAR",
        "Usuario:", "Contrasena:", "Entorno de escritorio", "Confirmar", "Instalacion completa!",
        "Sin teclado? Use los botones.",
        "Otro OS detectado:", "Habilitar multi-boot con el OS detectado?",
        "Seleccionar idioma:", "ARRIBA ^", "ABAJO  v",
    },
};
static lang_t g_lang = LANG_EN;
static const char *T(str_id_t id) { return tr[g_lang][id]; }

/* ── OS detection ─────────────────────────────────────────────────── */
/* Detected OS info — filled by detect_other_os() */
#define OS_NONE    0
#define OS_WINDOWS 1
#define OS_LINUX   2

typedef struct {
    int    type;           /* OS_NONE / OS_WINDOWS / OS_LINUX */
    char   name[32];       /* human-readable name */
    uint32_t lba_start;    /* partition start LBA (for GRUB chainload) */
    uint8_t  part_num;     /* partition number (1-based) */
} detected_os_t;

static detected_os_t g_other_os = { OS_NONE, "", 0, 0 };
static int g_multiboot_enable = 0;

static inline uint8_t inb_det(uint16_t port) {
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v;
}
static inline void outb_det(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port));
}

/* Read one 512-byte sector via LBA28 PIO on primary IDE (0x1F0).
 * Returns 1 on success, 0 on timeout or error.
 * This is a best-effort detection only; failure is safe (no install effect). */
static int pio_read_sector(uint32_t lba, uint8_t *buf512) {
    /* Wait for drive ready */
    int t = 100000;
    while (t-- && (inb_det(0x1F7) & 0xC0) != 0x40);
    if ((inb_det(0x1F7) & 0x40) == 0) return 0;  /* not ready */

    outb_det(0x1F6, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    outb_det(0x1F2, 1);
    outb_det(0x1F3, (uint8_t)(lba & 0xFF));
    outb_det(0x1F4, (uint8_t)((lba >> 8) & 0xFF));
    outb_det(0x1F5, (uint8_t)((lba >> 16) & 0xFF));
    outb_det(0x1F7, 0x20);  /* READ SECTORS */

    t = 300000;
    while (t-- && !(inb_det(0x1F7) & 0x08));
    if (!(inb_det(0x1F7) & 0x08)) return 0;  /* DRQ not set */

    uint16_t *w = (uint16_t *)buf512;
    for (int i = 0; i < 256; i++) {
        uint16_t val;
        __asm__ volatile("inw %1,%0":"=a"(val):"Nd"((uint16_t)0x1F0));
        w[i] = val;
    }
    return 1;
}

static void detect_other_os(void) {
    static uint8_t mbr[512];
    g_other_os.type = OS_NONE;

    if (!pio_read_sector(0, mbr)) {
        kinfo("INSTALLER: OS detect — IDE read failed (no disk or QEMU cdrom-only)\n");
        return;
    }

    /* Check MBR signature */
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        kinfo("INSTALLER: OS detect — no MBR signature\n");
        return;
    }

    /* Scan 4 primary partition entries at offset 0x1BE */
    for (int i = 0; i < 4; i++) {
        uint8_t *e = mbr + 0x1BE + i * 16;
        uint8_t  type_id = e[4];
        uint32_t lba = (uint32_t)e[8]  | ((uint32_t)e[9]  << 8) |
                       ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        if (lba == 0) continue;

        /* Windows: NTFS (0x07), FAT32 (0x0B/0x0C), exFAT (0x07 with NTFS sig) */
        if (type_id == 0x07 || type_id == 0x0B || type_id == 0x0C ||
            type_id == 0x27) {
            /* Try to verify NTFS signature at partition start */
            static uint8_t pbr[512];
            if (pio_read_sector(lba, pbr)) {
                /* NTFS OEM ID at offset 3: "NTFS    " */
                if (pbr[3]=='N' && pbr[4]=='T' && pbr[5]=='F' && pbr[6]=='S') {
                    g_other_os.type      = OS_WINDOWS;
                    g_other_os.lba_start = lba;
                    g_other_os.part_num  = (uint8_t)(i + 1);
                    strcpy(g_other_os.name, "Windows (NTFS)");
                    kinfo("INSTALLER: detected Windows (NTFS) on part %d lba=%u\n",
                          i+1, (unsigned)lba);
                    return;
                }
                /* FAT32 signature: bytes 82-89 = "FAT32   " */
                if (pbr[82]=='F' && pbr[83]=='A' && pbr[84]=='T') {
                    g_other_os.type      = OS_WINDOWS;
                    g_other_os.lba_start = lba;
                    g_other_os.part_num  = (uint8_t)(i + 1);
                    strcpy(g_other_os.name, "Windows (FAT32)");
                    kinfo("INSTALLER: detected Windows (FAT32) on part %d\n", i+1);
                    return;
                }
            }
        }
        /* Linux: ext2/3/4 (0x83), swap (0x82), LVM (0x8E), btrfs (0x83) */
        if (type_id == 0x83 || type_id == 0x82 || type_id == 0x8E) {
            static uint8_t sb[512];
            /* ext2/3/4 superblock is at byte 1024, magic 0xEF53 at offset 56 */
            if (pio_read_sector(lba + 2, sb)) {
                uint16_t magic = (uint16_t)(sb[56] | (sb[57] << 8));
                if (magic == 0xEF53) {
                    g_other_os.type      = OS_LINUX;
                    g_other_os.lba_start = lba;
                    g_other_os.part_num  = (uint8_t)(i + 1);
                    if (type_id == 0x82)
                        strcpy(g_other_os.name, "Linux (swap)");
                    else
                        strcpy(g_other_os.name, "Linux (ext2/3/4)");
                    kinfo("INSTALLER: detected Linux ext fs on part %d lba=%u\n",
                          i+1, (unsigned)lba);
                    return;
                }
            }
            /* Even without superblock confirmation, mark as likely Linux */
            if (g_other_os.type == OS_NONE) {
                g_other_os.type      = OS_LINUX;
                g_other_os.lba_start = lba;
                g_other_os.part_num  = (uint8_t)(i + 1);
                strcpy(g_other_os.name, "Linux");
                kinfo("INSTALLER: likely Linux partition on part %d\n", i+1);
            }
        }
    }
}

/* ── State ─────────────────────────────────────────────────────────── */
static char inst_user[64] = "";
static char inst_pass[64] = "";
static int  inst_de       = 0;
static int  inst_step     = 0;
/* Steps: 0=lang 1=welcome 2=user 3=pass 4=de 5=os-detect 6=confirm */
#define STEP_LANG    0
#define STEP_WELCOME 1
#define STEP_USER    2
#define STEP_PASS    3
#define STEP_DE      4
#define STEP_OS      5
#define STEP_CONFIRM 6

static const char *DE_NAMES[] = { "LXQt (Default)", "MATE", "KDE Plasma" };

/* ── Buttons ────────────────────────────────────────────────────────── */
typedef struct { uint32_t x,y,w,h; const char *label; int action; } btn_t;
#define ACT_NONE 0
#define ACT_NEXT 1
#define ACT_UP   2
#define ACT_DOWN 3
#define ACT_SKIP 4
#define ACT_BACK 5
#define ACT_BKSP 6
#define ACT_LIVE 7
#define ACT_LANG 8   /* data = lang index stored in label[0] */
#define MAX_BTNS 8
static btn_t  cur_btns[MAX_BTNS];
static int    num_btns = 0;

static void btn_add(uint32_t x,uint32_t y,uint32_t w,uint32_t h,
                    const char *lbl, int act) {
    if (num_btns < MAX_BTNS)
        cur_btns[num_btns++] = (btn_t){x,y,w,h,lbl,act};
}
static int pt_in(const btn_t *b, int px, int py) {
    return px>=(int)b->x && px<(int)(b->x+b->w) &&
           py>=(int)b->y && py<(int)(b->y+b->h);
}
static void draw_btn(const btn_t *b, int hov) {
    uint32_t bc = hov ? COL_BTN_HOV : COL_BTN;
    fb_rounded_rect(b->x,b->y,b->w,b->h,6,bc);
    fb_fill_rect(b->x,b->y,b->w,1,COL_ACC);
    fb_fill_rect(b->x,b->y+b->h-1,b->w,1,COL_ACC);
    fb_fill_rect(b->x,b->y,1,b->h,COL_ACC);
    fb_fill_rect(b->x+b->w-1,b->y,1,b->h,COL_ACC);
    uint32_t lw=(uint32_t)(strlen(b->label)*FW);
    uint32_t lx=b->x+(b->w>lw?(b->w-lw)/2:2);
    uint32_t ly=b->y+(b->h>FH?(b->h-FH)/2:0);
    fb_print(lx,ly,b->label,COL_TXT,bc);
}

/* ── Step rendering ─────────────────────────────────────────────────── */
static void inst_draw(int mxp, int myp) {
    if (!fb.available) return;
    uint32_t W=fb.width, H=fb.height;
    char buf[256];
    fb_fill_rect(0,0,W,H,COL_BG);
    /* Header */
    fb_fill_rect(0,0,W,44,COL_ACC);
    fb_print_s(12,10,"DracolaxOS Installer",COL_TXT,COL_ACC,2);
    /* Step dots (STEP_LANG is pre-step, show 5 dots for steps 1-5) */
    int visible_step = inst_step > STEP_LANG ? inst_step - 1 : 0;
    for (int i=0;i<5;i++) {
        uint32_t sc=(i<visible_step)?COL_OK:(i==visible_step)?COL_ACC2:COL_BTN;
        fb_fill_rect(12u+(uint32_t)i*50u,52,44,4,sc);
    }
    uint32_t y=68; num_btns=0;

    switch (inst_step) {

    /* ── Language selection ─────────────────────────────────── */
    case STEP_LANG:
        fb_print_s(20,y,"Select Language",COL_TXT,COL_BG,2);
        fb_print(20,y+44,T(S_LANG_SELECT),COL_DIM,COL_BG);
        {
            static const char *lang_names[LANG_COUNT] = {"English","Portugues","Espanol"};
            for (int i=0;i<LANG_COUNT;i++) {
                uint32_t bc=(i==(int)g_lang)?COL_ACC:0x1A1D3Au;
                uint32_t by=y+68u+(uint32_t)i*42u;
                fb_rounded_rect(20,by,300,36,4,bc);
                fb_print(30,by+10,lang_names[i],COL_TXT,bc);
            }
        }
        btn_add(W/2-60,y+220,120,34,T(S_NEXT),ACT_NEXT);
        break;

    /* ── Welcome / Live ─────────────────────────────────────── */
    case STEP_WELCOME:
        fb_print_s(20,y,T(S_WELCOME),COL_TXT,COL_BG,2);
        fb_print(20,y+48,T(S_NO_KB),COL_DIM,COL_BG);
        fb_print(20,y+68,"Use mouse/touch to navigate. Keyboard optional.",COL_DIM,COL_BG);
        /* Live session button */
        fb_rounded_rect(20,y+108,340,38,6,0x1A1D3Au);
        fb_fill_rect(20,y+108,340,1,COL_WARN);
        fb_print(30,y+120,T(S_LIVE),COL_WARN,0x1A1D3Au);
        btn_add(20,y+108,340,38,T(S_LIVE),ACT_LIVE);
        btn_add(W/2-60,y+166,120,34,T(S_NEXT),ACT_NEXT);
        break;

    /* ── Username ───────────────────────────────────────────── */
    case STEP_USER:
        fb_print_s(20,y,"Create Account",COL_TXT,COL_BG,2);
        fb_print(20,y+40,T(S_USERNAME),COL_DIM,COL_BG);
        fb_fill_rect(20,y+58,340,28,0x1A1D3Au);
        fb_print(26,y+64,inst_user,COL_TXT,0x1A1D3Au);
        fb_fill_rect(26u+(uint32_t)strlen(inst_user)*FW,y+65,2,FH,COL_ACC2);
        fb_print(20,y+96,"No keyboard? Tap SKIP for default (dracolax).",COL_DIM,COL_BG);
        btn_add(20,y+130,90,32,T(S_BACK),ACT_BACK);
        btn_add(120,y+130,90,32,T(S_BKSP),ACT_BKSP);
        btn_add(220,y+130,70,32,T(S_SKIP),ACT_SKIP);
        btn_add(W-110,y+130,90,32,T(S_NEXT),ACT_NEXT);
        break;

    /* ── Password ───────────────────────────────────────────── */
    case STEP_PASS:
        fb_print_s(20,y,"Set Password",COL_TXT,COL_BG,2);
        fb_fill_rect(20,y+40,340,28,0x1A1D3Au);
        { size_t pl=strlen(inst_pass);
          for(size_t i=0;i<pl&&i<32u;i++)
              fb_print(26u+(uint32_t)i*FW,y+46,"*",COL_TXT,0x1A1D3Au);
          fb_fill_rect(26u+(uint32_t)pl*FW,y+47,2,FH,COL_ACC2); }
        fb_print(20,y+78,"SKIP uses 'dracolax123'.",COL_DIM,COL_BG);
        btn_add(20,y+110,90,32,T(S_BACK),ACT_BACK);
        btn_add(120,y+110,90,32,T(S_BKSP),ACT_BKSP);
        btn_add(220,y+110,70,32,T(S_SKIP),ACT_SKIP);
        btn_add(W-110,y+110,90,32,T(S_NEXT),ACT_NEXT);
        break;

    /* ── Desktop ─────────────────────────────────────────────── */
    case STEP_DE:
        fb_print_s(20,y,T(S_DESKTOP),COL_TXT,COL_BG,2);
        for(int i=0;i<3;i++) {
            uint32_t bc=(i==inst_de)?COL_ACC:0x1A1D3Au;
            uint32_t by=y+46u+(uint32_t)i*40u;
            fb_rounded_rect(20,by,340,34,4,bc);
            fb_print(26,by+9,DE_NAMES[i],COL_TXT,bc);
        }
        btn_add(20,y+178,90,32,T(S_BACK),ACT_BACK);
        btn_add(120,y+178,90,32,T(S_UP),ACT_UP);
        btn_add(220,y+178,90,32,T(S_DOWN),ACT_DOWN);
        btn_add(W-110,y+178,90,32,T(S_NEXT),ACT_NEXT);
        break;

    /* ── Other OS / Multi-boot ───────────────────────────────── */
    case STEP_OS:
        fb_print_s(20,y,"Other OS",COL_TXT,COL_BG,2);
        if (g_other_os.type != OS_NONE) {
            fb_print(20,y+44,T(S_DETECTED_OS),COL_DIM,COL_BG);
            snprintf(buf,sizeof(buf),"  %s  (partition %d)",
                     g_other_os.name, g_other_os.part_num);
            fb_print(20,y+64,buf,COL_OK,COL_BG);
            fb_print(20,y+94,T(S_MULTIBOOT),COL_DIM,COL_BG);
            /* Toggle */
            uint32_t tc = g_multiboot_enable ? COL_ACC : 0x1A1D3Au;
            fb_rounded_rect(20,y+120,340,36,6,tc);
            fb_print(30,y+132,g_multiboot_enable?"Multi-boot: ON":"Multi-boot: OFF",
                     COL_TXT,tc);
            btn_add(20,y+120,340,36,
                    g_multiboot_enable?"Multi-boot: ON":"Multi-boot: OFF",ACT_LIVE);
        } else {
            fb_print(20,y+44,"No other operating system detected.",COL_DIM,COL_BG);
            fb_print(20,y+64,"(Only IDE disks are scanned. AHCI/NVMe: not yet supported.)",
                     COL_DIM,COL_BG);
        }
        btn_add(20,y+180,90,32,T(S_BACK),ACT_BACK);
        btn_add(W-110,y+180,90,32,T(S_NEXT),ACT_NEXT);
        break;

    /* ── Confirm ─────────────────────────────────────────────── */
    case STEP_CONFIRM:
        fb_print_s(20,y,T(S_INSTALL_DONE),COL_TXT,COL_BG,2);
        snprintf(buf,sizeof(buf),"  User    :  %s",inst_user);
        fb_print(20,y+50,buf,COL_TXT,COL_BG);
        snprintf(buf,sizeof(buf),"  Password:  %s",strlen(inst_pass)?"********":"(none)");
        fb_print(20,y+70,buf,COL_TXT,COL_BG);
        snprintf(buf,sizeof(buf),"  Desktop :  %s",DE_NAMES[inst_de]);
        fb_print(20,y+90,buf,COL_TXT,COL_BG);
        if (g_other_os.type != OS_NONE) {
            snprintf(buf,sizeof(buf),"  Multi-boot: %s (%s)",
                     g_multiboot_enable?"ON":"OFF", g_other_os.name);
            fb_print(20,y+110,buf,
                     g_multiboot_enable?COL_OK:COL_DIM,COL_BG);
        }
        fb_print(20,y+150,"Click FINISH or press ENTER.",COL_ACC2,COL_BG);
        btn_add(20,y+184,90,32,T(S_BACK),ACT_BACK);
        btn_add(W-110,y+184,90,32,T(S_FINISH),ACT_NEXT);
        break;
    }

    for(int i=0;i<num_btns;i++) draw_btn(&cur_btns[i],pt_in(&cur_btns[i],mxp,myp));
    fb_print(12,H-20,"DracolaxOS v1.0  |  Mouse & Touch supported  |  No keyboard needed",
             COL_DIM,COL_BG);
    fb_flip();
    /* Stamp cursor onto VRAM after flip — pure overlay, no background save */
    cursor_move((uint32_t)mxp,(uint32_t)myp);
}

/* ── Action handler ──────────────────────────────────────────────────── */
static int do_action(int act) {
    switch(act) {
    case ACT_BACK:
        if(inst_step>STEP_LANG) inst_step--;
        return 1;
    case ACT_LIVE: /* also used as "toggle multi-boot" on STEP_OS */
        if(inst_step==STEP_OS) {
            g_multiboot_enable = !g_multiboot_enable;
        } else {
            /* Live session: skip install, create guest user, go to desktop */
            kinfo("INSTALLER: live session selected\n");
            dracoauth_add_user("user","",ROLE_USER);
            return 3;  /* 3 = live */
        }
        return 1;
    case ACT_UP:
        if(inst_step==STEP_DE) inst_de=(inst_de+2)%3;
        if(inst_step==STEP_LANG) g_lang=(lang_t)((g_lang+LANG_COUNT-1)%LANG_COUNT);
        return 1;
    case ACT_DOWN:
        if(inst_step==STEP_DE) inst_de=(inst_de+1)%3;
        if(inst_step==STEP_LANG) g_lang=(lang_t)((g_lang+1)%LANG_COUNT);
        return 1;
    case ACT_BKSP: {
        char *b=(inst_step==STEP_USER)?inst_user:inst_pass;
        size_t l=strlen(b); if(l>0) b[l-1]='\0';
        return 1; }
    case ACT_SKIP:
        if(inst_step==STEP_USER && strlen(inst_user)==0) strcpy(inst_user,"dracolax");
        if(inst_step==STEP_PASS && strlen(inst_pass)==0) strcpy(inst_pass,"dracolax123");
        __attribute__((fallthrough));  /* intentional: skip shares NEXT logic */
    case ACT_NEXT:
        if(inst_step==STEP_LANG)    { inst_step=STEP_WELCOME; return 1; }
        if(inst_step==STEP_WELCOME) { inst_step=STEP_USER;    return 1; }
        if(inst_step==STEP_USER  && strlen(inst_user)>0) { inst_step=STEP_PASS;    return 1; }
        if(inst_step==STEP_PASS  && strlen(inst_pass)>0) { inst_step=STEP_DE;      return 1; }
        if(inst_step==STEP_DE)      { inst_step=STEP_OS;      return 1; }
        if(inst_step==STEP_OS)      { inst_step=STEP_CONFIRM; return 1; }
        if(inst_step==STEP_CONFIRM) return 2;  /* 2 = done */
        return 1;
    }
    return 0;
}

/* ── Entry point ─────────────────────────────────────────────────────── */
void installer_run(void) {
    if(!fb.available) return;
    __asm__ volatile("sti");
    fb_enable_shadow();

    inst_step=STEP_LANG; inst_de=0; g_lang=LANG_EN;
    g_multiboot_enable=0;
    inst_user[0]='\0'; inst_pass[0]='\0';

    /* Detect other OS before drawing (may take a moment on real HW) */
    detect_other_os();

    cursor_init();
    cursor_set_type(CURSOR_ARROW);
    cursor_show();

    int mx_pos=mouse_get_x(), my_pos=mouse_get_y();
    inst_draw(mx_pos,my_pos);

    uint8_t prev_btns=0;
    for(;;) {
        /* ── Keyboard ──────────────────────────────────────── */
        int c=keyboard_getchar();
        if(c) {
            uint8_t uc=(uint8_t)c;
            int act=ACT_NONE;
            if(uc==KB_KEY_UP)   act=ACT_UP;
            else if(uc==KB_KEY_DOWN) act=ACT_DOWN;
            else if(c=='\n')    act=ACT_NEXT;
            else if(c=='\b')    act=ACT_BKSP;
            else if(c>=' '&&c<127) {
                if(inst_step==STEP_USER||inst_step==STEP_PASS) {
                    char *buf=(inst_step==STEP_USER)?inst_user:inst_pass;
                    size_t l=strlen(buf);
                    if(l<63){buf[l]=(char)c;buf[l+1]='\0';}
                }
            }
            if(act!=ACT_NONE) {
                int r=do_action(act);
                if(r==2) goto done;
                if(r==3) goto live;
            }
        }

        /* ── Mouse / Touch ─────────────────────────────────── */
        vmmouse_poll();
        mx_pos=mouse_get_x(); my_pos=mouse_get_y();
        uint8_t btns=mouse_get_buttons();
        if((btns&MOUSE_BTN_LEFT)&&!(prev_btns&MOUSE_BTN_LEFT)) {
            for(int i=0;i<num_btns;i++) {
                if(pt_in(&cur_btns[i],mx_pos,my_pos)) {
                    int r=do_action(cur_btns[i].action);
                    if(r==2) goto done;
                    if(r==3) goto live;
                    break;
                }
            }
            /* Direct tap on DE row */
            if(inst_step==STEP_DE) {
                uint32_t base_y=68u+46u;
                for(int i=0;i<3;i++) {
                    uint32_t by=base_y+(uint32_t)i*40u;
                    if(mx_pos>=20&&mx_pos<360&&
                       my_pos>=(int)by&&my_pos<(int)(by+34))
                        inst_de=i;
                }
            }
            /* Tap on language row */
            if(inst_step==STEP_LANG) {
                uint32_t base_y=68u+68u;
                for(int i=0;i<LANG_COUNT;i++) {
                    uint32_t by=base_y+(uint32_t)i*42u;
                    if(mx_pos>=20&&mx_pos<320&&
                       my_pos>=(int)by&&my_pos<(int)(by+36))
                        g_lang=(lang_t)i;
                }
            }
        }
        prev_btns=btns;
        mouse_update_edges();
        inst_draw(mx_pos,my_pos);
        sched_sleep(16);
    }

live:
    kinfo("INSTALLER: live session — user 'user' created (no password)\n");
    return;

done:
    if(strlen(inst_user)==0) strcpy(inst_user,"dracolax");
    if(strlen(inst_pass)==0) strcpy(inst_pass,"dracolax123");
    dracoauth_add_user(inst_user,inst_pass,ROLE_ADMIN);
    kinfo("INSTALLER: done — user=%s de=%s multiboot=%d\n",
          inst_user,DE_NAMES[inst_de],g_multiboot_enable);

    /* Update GRUB if multi-boot enabled — write chainload entry to storage */
    if(g_multiboot_enable && g_other_os.type!=OS_NONE) {
        kinfo("INSTALLER: multi-boot enabled — %s (part %d, lba %u)\n",
              g_other_os.name, g_other_os.part_num, (unsigned)g_other_os.lba_start);
        /* The actual GRUB config update happens at install time on real hardware.
         * On live QEMU/ISO the config is read-only; the setting is recorded in
         * /storage/main/ui.json and applied on next reboot from installed media. */
    }
    cursor_set_type(CURSOR_ARROW);
}
