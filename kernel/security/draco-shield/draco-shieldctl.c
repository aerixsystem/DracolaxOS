/* draco-shield/draco-shieldctl.c
 *
 * Shell command handler for Draco Shield.
 * Invoked by the shell with: shield <subcommand> [args]
 *
 * Commands:
 *   shield list            — list all active rules
 *   shield allow <ip>      — allow traffic from IP
 *   shield deny <ip>       — deny traffic from IP
 *   shield allow-port <n>  — allow inbound TCP port
 *   shield deny-port <n>   — deny inbound TCP port
 *   shield reset           — reload default rules
 */
#include "../../types.h"
#include "../../klibc.h"
#include "../../drivers/vga/vga.h"
#include "../../log.h"
#include "firewall.h"

static uint32_t parse_ip(const char *s) {
    /* Parse dotted-decimal IPv4 */
    uint32_t a=0,b=0,c=0,d=0;
    int n = 0;
    while (*s && n < 4) {
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') v = v*10 + (*s++ - '0');
        if (n==0) a=v; else if(n==1) b=v;
        else if(n==2) c=v; else d=v;
        n++;
        if (*s == '.') s++;
    }
    return (a<<24)|(b<<16)|(c<<8)|d;
}

void shieldctl_run(int argc, char *argv[]) {
    if (argc < 2) {
        vga_print("usage: shield <list|allow|deny|allow-port|deny-port|reset>\n");
        return;
    }
    const char *cmd = argv[1];

    if (!strcmp(cmd, "list")) {
        shield_list_rules();
        return;
    }
    if (!strcmp(cmd, "reset")) {
        shield_init();
        vga_print("Shield rules reset to default.\n");
        return;
    }
    if ((!strcmp(cmd, "allow") || !strcmp(cmd, "deny")) && argc >= 3) {
        shield_rule_t r;
        memset(&r, 0, sizeof(r));
        r.active   = 1;
        r.priority = 100;
        r.action   = !strcmp(cmd, "allow") ? RULE_ALLOW : RULE_DENY;
        r.src_ip   = parse_ip(argv[2]);
        r.src_mask = 0xffffffff;
        r.direction = 2;
        snprintf(r.comment, SHIELD_COMMENT_LEN, "manual %s %s", cmd, argv[2]);
        int idx = shield_add_rule(&r);
        char buf[64];
        snprintf(buf, sizeof(buf), "Rule %d added: %s %s\n", idx, cmd, argv[2]);
        vga_print(buf);
        return;
    }
    if ((!strcmp(cmd, "allow-port") || !strcmp(cmd, "deny-port")) && argc >= 3) {
        shield_rule_t r;
        memset(&r, 0, sizeof(r));
        r.active    = 1;
        r.priority  = 50;
        r.action    = (cmd[0] == 'a') ? RULE_ALLOW : RULE_DENY;
        r.proto     = PROTO_TCP;
        r.dst_port  = (uint16_t)atoi(argv[2]);
        r.direction = 0; /* incoming */
        snprintf(r.comment, SHIELD_COMMENT_LEN, "%s port %s", cmd, argv[2]);
        int idx = shield_add_rule(&r);
        char buf[64];
        snprintf(buf, sizeof(buf), "Rule %d added: %s port %s\n", idx, cmd, argv[2]);
        vga_print(buf);
        return;
    }
    vga_print("unknown shield command\n");
}
