/* apps/appman/apps.c — DracolaxOS built-in applications
 *
 * v3.0 — Terminal only. All other apps stripped.
 * Every app MUST:
 *   1. Start with  __asm__ volatile("sti")
 *   2. Create a compositor window
 *   3. Loop until user exits (keeps window alive)
 *   4. Call comp_destroy_window() then sched_exit()
 */
#include "../../kernel/types.h"
#include "../../kernel/klibc.h"
#include "../../kernel/log.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/fs/vfs.h"
#include "../../kernel/fs/ramfs.h"
#include "../../kernel/drivers/ps2/keyboard.h"
#include "../../kernel/drivers/ps2/mouse.h"
#include "../../kernel/drivers/ps2/input_router.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/vmm.h"
#include "../../kernel/drivers/vga/fb.h"
#include "../../kernel/security/dracoauth.h"
#include "../../kernel/limits.h"
#include "../../kernel/security/dracolock.h"
#include "../../kernel/security/dracolicence.h"
#include "../../kernel/security/draco-shield/firewall.h"
#include "../../drx/cli/draco-install.h"
#include "../../gui/compositor/compositor.h"
#include "appman.h"

extern vfs_node_t *ramfs_root;
extern vfs_node_t *storage_root;

/* ── Terminal window size ─────────────────────────────────────────── */
#define TERM_WIN_W  480u
#define TERM_WIN_H  320u

/* ========================================================================
 * Terminal — a working command-line window
 *
 * Layout:
 *   Compositor window with dark green background.
 *   Uses comp_term_t for text I/O — all output goes to the backbuf,
 *   rendered by the desktop compositor each frame.
 *   Input comes from this task's input_router queue (set focused by
 *   appman_launch before the task starts).
 *
 * Built-in commands: help, ls, pwd, cd, cat, echo, mem, tasks, clear, exit
 * ======================================================================*/
static int   g_term_win = -1;
static comp_term_t g_term;

/* Simple path state */
static char g_cwd[128] = "/ramfs";

static void term_print(const char *s) {
    comp_win_term_print(&g_term, s);
}
static void term_printf(const char *fmt, ...) {
    char buf[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    comp_win_term_print(&g_term, buf);
}

/* Resolve path relative to cwd */
static void resolve(const char *in, char *out, size_t sz) {
    if (!in || !in[0]) { strncpy(out, g_cwd, sz-1); out[sz-1]='\0'; return; }
    if (in[0] == '/') { strncpy(out, in, sz-1); out[sz-1]='\0'; return; }
    snprintf(out, sz, "%s/%s", g_cwd, in);
}

static void cmd_ls(const char *arg) {
    char path[128]; resolve(arg, path, sizeof(path));
    vfs_node_t *dir = vfs_open(path);
    if (!dir) {
        if (strncmp(path,"/ramfs",6)==0)       dir = ramfs_root;
        else if (strncmp(path,"/storage",8)==0) dir = storage_root;
    }
    if (!dir) { term_printf("ls: not found: %s\n", path); return; }
    char name[VFS_NAME_MAX];
    int n = 0;
    for (uint32_t i = 0; ; i++) {
        if (vfs_readdir(dir, i, name, sizeof(name)) != 0) break;
        vfs_node_t *ch = vfs_finddir(dir, name);
        term_printf("  %s%s\n", name, (ch && ch->type==VFS_TYPE_DIR)?"/":"");
        n++;
    }
    if (!n) term_print("  (empty)\n");
}

static void cmd_cat(const char *arg) {
    char path[128]; resolve(arg, path, sizeof(path));
    vfs_node_t *f = vfs_open(path);
    if (!f) {
        /* try ramfs direct */
        const char *rel = arg;
        if (ramfs_root) f = vfs_finddir(ramfs_root, rel);
    }
    if (!f) { term_printf("cat: not found: %s\n", arg); return; }
    uint8_t buf[1024]; int r = vfs_read(f, 0, sizeof(buf)-1, buf);
    if (r > 0) { buf[r]='\0'; term_print((char*)buf); term_print("\n"); }
    else term_print("(empty)\n");
}

static void cmd_mem(void) {
    limits_update();
    uint32_t tkb = (uint32_t)(pmm_total_bytes()/1024);
    uint32_t ukb = pmm_used_pages()*4;
    uint32_t fkb = pmm_free_pages()*4;
    term_printf("Physical: %u KB total  %u KB used  %u KB free\n", tkb, ukb, fkb);
    term_printf("VMM heap: %u KB used\n", vmm_heap_used()/1024);
}

static void cmd_tasks(void) {
    term_print("ID  NAME             STATE\n");
    for (int i = 0; i < TASK_MAX; i++) {
        task_t *t = sched_task_at(i);
        if (!t || t->state == 0) continue;
        static const char *st[]={"EMPTY","READY","RUN  ","SLEEP","DEAD "};
        term_printf("%-3d %-16s %s\n", i, t->name,
                    t->state<=4?st[t->state]:"?");
    }
    term_printf("Total: %d tasks\n", sched_task_count());
}

static void cmd_help(void) {
    term_print(
        "DracolaxOS Terminal — built-in commands:\n"
        "  ls [path]     list directory\n"
        "  pwd           print working directory\n"
        "  cd <path>     change directory\n"
        "  cat <file>    print file contents\n"
        "  echo <text>   print text\n"
        "  mem           show memory usage\n"
        "  tasks         list running tasks\n"
        "  clear         clear terminal\n"
        "  exit          close terminal\n"
    );
}

void app_terminal(void) {
    __asm__ volatile ("sti");

    /* Position terminal slightly offset so multiple terminals don't overlap */
    static int g_term_count = 0;
    int off = (g_term_count++ % 4) * 24;

    uint32_t wx = (fb.width  > TERM_WIN_W) ? (fb.width  - TERM_WIN_W)/2 + (uint32_t)off : 0;
    uint32_t wy = (fb.height > TERM_WIN_H) ? (fb.height - TERM_WIN_H)/2 + (uint32_t)off : 0;

    g_term_win = comp_create_window("Terminal", wx, wy, TERM_WIN_W, TERM_WIN_H);
    if (g_term_win < 0) {
        kinfo("TERMINAL: comp_create_window failed\n");
        sched_exit();
        return;
    }

    /* Initialise terminal — bright green on very dark green */
    comp_win_term_init(&g_term, g_term_win, 0x40FF80u, 0x050D06u);

    term_print("DracolaxOS Terminal v1\n");
    term_print("Type 'help' for commands.\n\n");

    /* Main command loop */
    while (1) {
        term_printf("%s$ ", g_cwd);
        char line[256];
        int r = comp_win_term_readline(&g_term, line, sizeof(line));
        if (r < 0) break;   /* ESC pressed */
        if (!line[0]) continue;

        /* Parse command and first argument */
        char *argv[8]; int argc = 0;
        char lcopy[256]; strncpy(lcopy, line, sizeof(lcopy)-1); lcopy[255]='\0';
        char *p = lcopy;
        while (*p && argc < 8) {
            while (*p==' ') p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p!=' ') p++;
            if (*p) *p++ = '\0';
        }
        if (!argc) continue;

        const char *cmd = argv[0];
        const char *arg = argc>1 ? argv[1] : "";

        if (!strcmp(cmd,"exit") || !strcmp(cmd,"quit")) break;
        else if (!strcmp(cmd,"help"))  cmd_help();
        else if (!strcmp(cmd,"clear")) {
            comp_win_term_init(&g_term, g_term_win, 0x40FF80u, 0x050D06u);
        }
        else if (!strcmp(cmd,"ls"))    cmd_ls(arg);
        else if (!strcmp(cmd,"pwd"))   term_printf("%s\n", g_cwd);
        else if (!strcmp(cmd,"cd")) {
            if (!arg[0] || !strcmp(arg,"~")) {
                strncpy(g_cwd, "/ramfs", sizeof(g_cwd)-1);
            } else if (!strcmp(arg,"..")) {
                char *last = strrchr(g_cwd, '/');
                if (last && last != g_cwd) *last='\0';
                else strncpy(g_cwd, "/ramfs", sizeof(g_cwd)-1);
            } else {
                char newpath[128]; resolve(arg, newpath, sizeof(newpath));
                /* Accept if starts with known mount */
                if (strncmp(newpath,"/ramfs",6)==0 ||
                    strncmp(newpath,"/storage",8)==0 ||
                    strncmp(newpath,"/proc",5)==0) {
                    strncpy(g_cwd, newpath, sizeof(g_cwd)-1);
                } else {
                    term_printf("cd: no such directory: %s\n", arg);
                }
            }
        }
        else if (!strcmp(cmd,"cat"))   { if (!arg[0]) term_print("usage: cat <file>\n"); else cmd_cat(arg); }
        else if (!strcmp(cmd,"echo")) {
            for (int i=1;i<argc;i++){if(i>1)term_print(" ");term_print(argv[i]);}
            term_print("\n");
        }
        else if (!strcmp(cmd,"mem"))   cmd_mem();
        else if (!strcmp(cmd,"tasks")) cmd_tasks();
        else term_printf("command not found: %s\n", cmd);
    }

    term_print("\n[Terminal closed]\n");
    sched_sleep(800);   /* brief pause so user can read last line */

    input_router_set_focus(0); /* return keyboard to desktop before exiting */
    comp_destroy_window(g_term_win);
    g_term_win = -1;
    sched_exit();
}

/* ── Macro: open a 480×320 window with a comp_term_t and print a header ── */
#define APP_W 480u
#define APP_H 320u

static int _app_open(const char *title, comp_term_t *t, uint32_t fg, uint32_t bg) {
    uint32_t wx = fb.width  > APP_W ? (fb.width  - APP_W) / 2 : 0;
    uint32_t wy = fb.height > APP_H ? (fb.height - APP_H) / 2 : 0;
    int win = comp_create_window(title, wx, wy, APP_W, APP_H);
    if (win < 0) { kinfo("APP: comp_create_window failed for '%s'\n", title); return -1; }
    comp_win_term_init(t, win, fg, bg);
    comp_win_term_printf(t, "=== %s ===\n\n", title);
    return win;
}

/* ======================================================================== */
void app_text_editor(void) {
    __asm__ volatile ("sti");
    comp_term_t t; int win = _app_open("Text Editor", &t, 0xD0D8FFu, 0x0A0E1Eu);
    if (win < 0) { sched_exit(); return; }
    comp_win_term_print(&t, "Filename: ");
    char fname[64];
    if (comp_win_term_readline(&t, fname, sizeof(fname)) < 0 || !fname[0]) goto done_te;
    {
        char buf[2048]; int len = 0;
        vfs_node_t *f = vfs_finddir(ramfs_root, fname);
        if (f) { int n = vfs_read(f, 0, sizeof(buf)-1, (uint8_t*)buf); if (n>0){len=n;buf[n]='\0';} }
        buf[len] = '\0';
        comp_win_term_printf(&t, "--- %s ---\n%s\n--- Append (empty line=save, ESC=cancel) ---\n", fname, buf);
        while (1) {
            char line[256]; int r = comp_win_term_readline(&t, line, sizeof(line));
            if (r < 0) break;
            if (r == 0) break;
            if (len + r + 2 < (int)sizeof(buf)-1) { strncat(buf, line, sizeof(buf)-len-2); buf[len+r]='\n'; len+=r+1; buf[len]='\0'; }
        }
        if (!f) { ramfs_create(ramfs_root, fname); f = vfs_finddir(ramfs_root, fname); }
        if (f) { vfs_write(f, 0, (uint32_t)len, (const uint8_t*)buf); comp_win_term_print(&t, "\nSaved.\n"); sched_sleep(800); }
    }
done_te:
    input_router_set_focus(0);
    comp_destroy_window(win); sched_exit();
}

void app_file_manager(void) {
    __asm__ volatile ("sti");
    comp_term_t t; int win = _app_open("File Manager", &t, 0xD0D8FFu, 0x0A0E1Eu);
    if (win < 0) { sched_exit(); return; }
    comp_win_term_print(&t, "Commands: ls, open <f>, del <f>, exit\n\n");
    while (1) {
        comp_win_term_print(&t, "fm> "); char cmd[128];
        int r = comp_win_term_readline(&t, cmd, sizeof(cmd));
        if (r < 0 || !strcmp(cmd,"exit") || !strcmp(cmd,"quit")) break;
        if (!strcmp(cmd,"ls") || !cmd[0]) {
            comp_win_term_print(&t, "/ramfs:\n");
            char name[64];
            for (uint32_t i = 0; ; i++) { if (vfs_readdir(ramfs_root, i, name, sizeof(name))!=0) break; comp_win_term_printf(&t, "  %s\n", name); }
        } else if (!strncmp(cmd,"open ",5)) {
            vfs_node_t *f = vfs_finddir(ramfs_root, cmd+5);
            if (!f) { comp_win_term_print(&t, "Not found.\n"); continue; }
            uint8_t buf[512]; int n = vfs_read(f, 0, sizeof(buf)-1, buf);
            if (n>0){buf[n]='\0'; comp_win_term_print(&t,(char*)buf); comp_win_term_print(&t,"\n");}
        } else if (!strncmp(cmd,"del ",4)) {
            ramfs_delete(ramfs_root, cmd+4); comp_win_term_print(&t, "Deleted.\n");
        } else comp_win_term_print(&t, "Commands: ls, open <f>, del <f>, exit\n");
    }
    input_router_set_focus(0);
    comp_destroy_window(win); sched_exit();
}

void app_system_monitor(void) {
    __asm__ volatile ("sti");
    comp_term_t t; int win = _app_open("System Monitor", &t, 0x80FFD0u, 0x050E0Au);
    if (win < 0) { sched_exit(); return; }
    int tid = sched_current_id();
    while (1) {
        limits_update();
        uint32_t tkb=(uint32_t)(pmm_total_bytes()/1024), ukb=pmm_used_pages()*4;
        uint32_t tck=pit_ticks(), s=tck/100, m=s/60, h=m/60; s%=60; m%=60;
        comp_win_term_init(&t, win, 0x80FFD0u, 0x050E0Au);
        comp_win_term_print(&t, "=== System Monitor ===\n\n");
        comp_win_term_printf(&t, "Uptime : %02u:%02u:%02u\nMemory : %u/%u KB (%u%%)\nTasks  : %d\n\n",
            h,m,s, ukb,tkb, tkb?(ukb*100)/tkb:0, sched_task_count());
        comp_win_term_print(&t, "PID  NAME             STATE\n");
        for (int i=0;i<TASK_MAX;i++){task_t *t2=sched_task_at(i); if(!t2||!t2->state) continue; static const char *st[]={"","READY","RUN","SLEEP","DEAD"}; comp_win_term_printf(&t,"%-4d %-16s %s\n",i,t2->name,t2->state<5?st[t2->state]:"?");}
        comp_win_term_print(&t, "\nESC=quit  any key=refresh\n");
        sched_sleep(500);
        int c = input_router_getchar(tid);
        if (c == 0x1B) break;
    }
    input_router_set_focus(0);
    comp_destroy_window(win); sched_exit();
}

static int _calc_eval(const char *e){ int a=0,b=0; char op=0; const char *p=e; while(*p==' ')p++; a=atoi(p); while(*p&&*p!='+'&&*p!='-'&&*p!='*'&&*p!='/') p++; if(*p){op=*p;p++;b=atoi(p);} switch(op){case '+':return a+b;case '-':return a-b;case '*':return a*b;case '/':return b?a/b:0;} return a; }

void app_calculator(void) {
    __asm__ volatile ("sti");
    comp_term_t t; int win = _app_open("Calculator", &t, 0xFFD0A0u, 0x100800u);
    if (win < 0) { sched_exit(); return; }
    comp_win_term_print(&t, "Enter expression: 12 + 34, 6 * 7, etc.\nType 'exit' to quit.\n\n");
    while (1) {
        comp_win_term_print(&t, "calc> "); char ex[64];
        int r = comp_win_term_readline(&t, ex, sizeof(ex));
        if (r < 0 || !strcmp(ex,"exit")) break;
        if (!ex[0]) continue;
        comp_win_term_printf(&t, "= %d\n", _calc_eval(ex));
    }
    input_router_set_focus(0);
    comp_destroy_window(win); sched_exit();
}

void app_settings(void) {
    __asm__ volatile ("sti");
    comp_term_t t; int win = _app_open("Settings", &t, 0xD0D8FFu, 0x0A0E1Eu);
    if (win < 0) { sched_exit(); return; }
    comp_win_term_print(&t, "1. Volume\n2. Brightness\n3. User info\n4. Device ID\n5. Exit\n\n");
    while (1) {
        comp_win_term_print(&t, "settings> "); char line[32]; comp_win_term_readline(&t, line, sizeof(line));
        if (!strcmp(line,"1")) comp_win_term_printf(&t, "Volume: %u%%\n", limits_get_volume());
        else if (!strcmp(line,"2")) comp_win_term_printf(&t, "Brightness: %u%%\n", g_limits.brightness_pct);
        else if (!strcmp(line,"3")) comp_win_term_printf(&t, "User: %s\n", dracoauth_whoami());
        else if (!strcmp(line,"4")) comp_win_term_printf(&t, "Device: %s\nLicence: %s\n", dracolicence_device_id(), dracolicence_licence_id());
        else if (!strcmp(line,"5")||!strcmp(line,"exit")||!line[0]) break;
    }
    input_router_set_focus(0);
    comp_destroy_window(win); sched_exit();
}

void app_pkg_manager(void) {
    __asm__ volatile ("sti");
    comp_term_t t; int win = _app_open("Package Manager", &t, 0xD0FFD0u, 0x040E04u);
    if (win < 0) { sched_exit(); return; }
    comp_win_term_print(&t, "Commands: list, install <f>, remove <n>, exit\n\n");
    while (1) {
        comp_win_term_print(&t, "pkg> "); char line[128]; int r = comp_win_term_readline(&t, line, sizeof(line));
        if (r < 0 || !strcmp(line,"exit")) break;
        if (!line[0]) continue;
        char *argv[8]; int argc=0; argv[argc++]=(char*)"draco";
        char *p=line; while(*p&&argc<8){while(*p==' ')p++;if(!*p)break;argv[argc++]=p;while(*p&&*p!=' ')p++;if(*p)*p++='\0';}
        draco_install_run(argc, argv);
    }
    input_router_set_focus(0);
    comp_destroy_window(win); sched_exit();
}

void app_shield_ui(void) {
    __asm__ volatile ("sti");
    comp_term_t t; int win = _app_open("Draco Shield", &t, 0xFFB0B0u, 0x0E0404u);
    if (win < 0) { sched_exit(); return; }
    comp_win_term_print(&t, "Commands: list, reset, allow/deny <ip>, allow-port/deny-port <n>, exit\n\n");
    while (1) {
        comp_win_term_print(&t, "shield> "); char line[128]; int r = comp_win_term_readline(&t, line, sizeof(line));
        if (r < 0 || !strcmp(line,"exit")) break;
        if (!line[0]) continue;
        char *argv[8]; int argc=0; argv[argc++]=(char*)"shield";
        char *p=line; while(*p&&argc<8){while(*p==' ')p++;if(!*p)break;argv[argc++]=p;while(*p&&*p!=' ')p++;if(*p)*p++='\0';}
        extern void shieldctl_run(int, char**); shieldctl_run(argc, argv);
    }
    input_router_set_focus(0);
    comp_destroy_window(win); sched_exit();
}

void app_draco_manager(void) {
    __asm__ volatile ("sti");
    comp_term_t t; int win = _app_open("Draco Manager", &t, 0xD0B0FFu, 0x08040Eu);
    if (win < 0) { sched_exit(); return; }
    comp_win_term_print(&t, "1.Info 2.Security 3.Users 4.Licence 5.Limits 0.Exit\n\n");
    while (1) {
        comp_win_term_print(&t, "draco> "); char line[32]; comp_win_term_readline(&t, line, sizeof(line));
        if (!strcmp(line,"1")){uint32_t tkb=(uint32_t)(pmm_total_bytes()/1024);uint32_t tck=pit_ticks(),s=tck/100,m=s/60,h=m/60;s%=60;m%=60;comp_win_term_printf(&t,"OS: DracolaxOS v1\nUptime: %02u:%02u:%02u  RAM: %u KB\n",h,m,s,tkb);}
        else if (!strcmp(line,"2")) comp_win_term_printf(&t,"Auth: %s\n",g_session.logged_in?"LOGGED IN":"NOT LOGGED IN");
        else if (!strcmp(line,"3")){char b[512];dracoauth_list_users(b,sizeof(b));comp_win_term_print(&t,b);}
        else if (!strcmp(line,"4")) comp_win_term_printf(&t,"Device: %s\nLicence: %s\n",dracolicence_device_id(),dracolicence_licence_id());
        else if (!strcmp(line,"5")) limits_print_status();
        else if (!strcmp(line,"0")||!strcmp(line,"exit")) break;
    }
    input_router_set_focus(0);
    comp_destroy_window(win); sched_exit();
}

void app_login_manager(void) {
    __asm__ volatile ("sti");
    comp_term_t t; int win = _app_open("Login Manager", &t, 0xC0C0FFu, 0x06060Eu);
    if (win < 0) { sched_exit(); return; }
    int tid = sched_current_id();
    for (int attempts=0; attempts<3; attempts++) {
        char uname[32], pass[64];
        comp_win_term_print(&t, "Username: "); if (comp_win_term_readline(&t,uname,sizeof(uname))<0) break;
        comp_win_term_print(&t, "Password: "); int pos=0; pass[0]='\0';
        while (pos<63){ int c; while((c=input_router_getchar(tid))<0)sched_yield(); if(c=='\n'||c=='\r'){pass[pos]='\0';comp_win_term_print(&t,"\n");break;} if(c=='\b'){if(pos>0)pos--;continue;} if((unsigned char)c>=0x20){pass[pos++]=c;comp_win_term_print(&t,"*");} }
        if (dracoauth_login(uname,pass)==0){ comp_win_term_printf(&t,"\nWelcome, %s!\n",uname); sched_sleep(1000); break; }
        comp_win_term_printf(&t,"Incorrect. (%d/3)\n",attempts+1);
    }
    input_router_set_focus(0);
    comp_destroy_window(win); sched_exit();
}

void app_paint(void) {
    __asm__ volatile ("sti");
    uint32_t wx=(fb.width>APP_W)?(fb.width-APP_W)/2:0, wy=(fb.height>APP_H)?(fb.height-APP_H)/2:0;
    int win=comp_create_window("Paint",wx,wy,APP_W,APP_H); if(win<0){sched_exit();return;}
    comp_window_fill(win,0,0,APP_W,APP_H,0xFFFFFFu);
    comp_window_print(win,8,4,"Paint  [+/-] brush  [Q] quit  click=draw",0x7828C8u);
    static const uint32_t pal[]={0xFF0000u,0x00CC00u,0x0055FFu,0xFFEE00u,0xFF00FFu,0x00FFFFu,0xFF8800u,0x000000u,0xFFFFFFu};
    static const int PN=9; uint32_t paly=APP_H-44u;
    comp_window_fill(win,0,paly-4u,APP_W,48u,0x1A1D3Au);
    for(int i=0;i<PN;i++) comp_window_fill(win,8u+(uint32_t)i*48u,paly,40u,36u,pal[i]);
    uint32_t cur_col=0x000000u; int brush=3, tid=sched_current_id();
    while(1){
        sched_sleep(20); mouse_update_edges();
        int mx=mouse_get_x(), my=mouse_get_y();
        int c=input_router_getchar(tid);
        if(c=='q'||c=='Q'||c==0x1B) break;
        if((c=='+'||c=='=')&&brush<15) brush++;
        if((c=='-'||c=='_')&&brush>1) brush--;
        if(mouse_btn_pressed(MOUSE_BTN_LEFT)){
            int lx=mx-(int)wx, ly=my-(int)wy;
            if(ly>=(int)paly-4&&ly<(int)APP_H){for(int i=0;i<PN;i++){int px2=8+i*48;if(lx>=px2&&lx<px2+40)cur_col=pal[i];}}
            else if(lx>=0&&ly>=24&&lx<(int)APP_W&&ly<(int)paly){for(int dy2=-brush;dy2<=brush;dy2++)for(int dx2=-brush;dx2<=brush;dx2++)if(dx2*dx2+dy2*dy2<=brush*brush){int bx=lx+dx2,by2=ly+dy2;if(bx>=0&&by2>=0&&bx<(int)APP_W&&by2<(int)APP_H)comp_window_fill(win,(uint32_t)bx,(uint32_t)by2,1u,1u,cur_col);}}
        }
    }
    input_router_set_focus(0);
    comp_destroy_window(win); sched_exit();
}

void app_image_viewer(void) {
    __asm__ volatile ("sti");
    comp_term_t t; int win=_app_open("Image Viewer",&t,0xA0D0FFu,0x04080Eu); if(win<0){sched_exit();return;}
    comp_window_fill(win,0,0,APP_W,APP_H,0x0A0A14u);
    comp_window_print(win,8,4,"Image Viewer - Colour Test Chart",0xF0F0FFu);
    for(uint32_t dx=0;dx<APP_W-16u;dx++){uint8_t t2=(uint8_t)(dx*255u/(APP_W-16u));comp_window_fill(win,8u+dx,28u,1u,28u,fb_color(t2,0,255-t2));}
    for(uint32_t dx=0;dx<APP_W-16u;dx++){uint8_t t2=(uint8_t)(dx*255u/(APP_W-16u));comp_window_fill(win,8u+dx,60u,1u,20u,fb_color(t2,t2,t2));}
    uint32_t bars[]={0xFF0000u,0x00FF00u,0x0000FFu,0x00FFFFu,0xFF00FFu,0xFFFF00u,0xFFFFFFu};
    uint32_t bw=(APP_W-16u)/7u; for(int i=0;i<7;i++) comp_window_fill(win,8u+(uint32_t)i*bw,84u,bw-2u,40u,bars[i]);
    comp_win_term_print(&t,"\n\n\n\n\n\n\nPress any key to close...");
    int tid=sched_current_id(); while(input_router_getchar(tid)<0) sched_yield();
    input_router_set_focus(0);
    comp_destroy_window(win); sched_exit();
}

void app_media_player(void) {
    __asm__ volatile ("sti");
    uint32_t wx=(fb.width>APP_W)?(fb.width-APP_W)/2:0, wy=(fb.height>APP_H)?(fb.height-APP_H)/2:0;
    int win=comp_create_window("Media Player",wx,wy,APP_W,APP_H); if(win<0){sched_exit();return;}
    extern uint32_t audio_get_volume(void); extern void audio_set_volume(uint32_t); extern void audio_mute(int); extern int audio_is_muted(void);
    uint32_t vol=audio_get_volume(), frame=0; int tid=sched_current_id();
    while(1){
        comp_window_fill(win,0,0,APP_W,APP_H,0x07070Fu);
        comp_window_print(win,8,4,"Media Player",0xF0F0FFu);
        char status[80]; snprintf(status,sizeof(status),"Vol:%u%% %s  +/-=vol M=mute Q=quit",vol,audio_is_muted()?"[MUTED]":"");
        comp_window_print(win,8,24,status,0xA0A0C8u);
        uint32_t bw2=(APP_W-16u)/24u;
        for(int b=0;b<24;b++){uint32_t seed=(uint32_t)(b*137+frame*31);seed^=seed>>13;seed*=0x45d9f3bu;seed^=seed>>15;uint32_t amp=20u+(seed%120u);uint32_t bx2=8u+(uint32_t)b*bw2,by2=200u-amp;comp_window_fill(win,bx2,by2,bw2-2u,amp,fb_color((uint8_t)(amp*180u/120u),20,(uint8_t)(255u-amp)));}
        uint32_t vw=(APP_W-90u)*vol/100u; comp_window_fill(win,70u,220u,APP_W-90u,12u,0x1A1A2Au); comp_window_fill(win,70u,220u,vw,12u,audio_is_muted()?0x444444u:0x7828C8u);
        comp_window_print(win,8,APP_H-20u,"+/- vol  M mute  Q quit",0x404060u);
        frame++; sched_sleep(80);
        int c=input_router_getchar(tid);
        if(c=='q'||c=='Q'||c==0x1B) break;
        if(c=='+'||c=='='){if(vol<100){vol+=5;audio_set_volume(vol);}}
        if(c=='-'||c=='_'){if(vol>0){vol-=5;audio_set_volume(vol);}}
        if(c=='m'||c=='M') audio_mute(!audio_is_muted());
    }
    input_router_set_focus(0);
    comp_destroy_window(win); sched_exit();
}
