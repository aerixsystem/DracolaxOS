/* draco-shield/firewall.c — Draco Shield packet filter implementation */
#include "../../types.h"
#include "../../klibc.h"
#include "../../log.h"
#include "../../drivers/vga/vga.h"
#include "firewall.h"
#include "../../fs/vfs.h"
#include "../../fs/ramfs.h"

static shield_rule_t rules[SHIELD_MAX_RULES];
static int rule_count = 0;

/* ---- helpers ------------------------------------------------------------ */

static int ip_match(uint32_t pkt_ip, uint32_t rule_ip, uint32_t mask) {
    if (rule_ip == 0) return 1; /* 0 = wildcard */
    return (pkt_ip & mask) == (rule_ip & mask);
}

/* ---- default rule set --------------------------------------------------- */

static void add_default_rules(void) {
    shield_rule_t r;
    memset(&r, 0, sizeof(r));

    /* Rule 0: Allow all loopback */
    r.active   = 1; r.priority = 0; r.action = RULE_ALLOW;
    r.src_ip   = 0x7f000001; /* 127.0.0.1 */
    r.src_mask = 0xff000000;
    strncpy(r.comment, "allow loopback", SHIELD_COMMENT_LEN);
    shield_add_rule(&r);

    /* Rule 1: Allow established connections (stateful) */
    memset(&r, 0, sizeof(r));
    r.active   = 1; r.priority = 10; r.action = RULE_ALLOW;
    r.stateful = 1;
    strncpy(r.comment, "allow established", SHIELD_COMMENT_LEN);
    shield_add_rule(&r);

    /* Rule 2: Deny all incoming by default */
    memset(&r, 0, sizeof(r));
    r.active    = 1; r.priority = 9999; r.action = RULE_DENY;
    r.direction = 0; /* incoming */
    strncpy(r.comment, "deny all incoming (default)", SHIELD_COMMENT_LEN);
    shield_add_rule(&r);

    kinfo("SHIELD: %d default rules loaded\n", rule_count);
}

/* ---- public API --------------------------------------------------------- */

void shield_init(void) {
    memset(rules, 0, sizeof(rules));
    rule_count = 0;
    add_default_rules();
    kinfo("SHIELD: Draco Shield initialised\n");
}

int shield_add_rule(const shield_rule_t *rule) {
    if (rule_count >= SHIELD_MAX_RULES) return -1;
    memcpy(&rules[rule_count], rule, sizeof(shield_rule_t));
    rules[rule_count].active = 1;
    return rule_count++;
}

int shield_remove_rule(int idx) {
    if (idx < 0 || idx >= rule_count) return -1;
    rules[idx].active = 0;
    return 0;
}

void shield_list_rules(void) {
    char buf[128];
    vga_print("Draco Shield Rules:\n");
    vga_print("  #   Pri  Action  Proto  Src->Dst   Dir  Comment\n");
    vga_print("  --  ---  ------  -----  ---------  ---  -------\n");
    for (int i = 0; i < rule_count; i++) {
        if (!rules[i].active) continue;
        snprintf(buf, sizeof(buf), "  %2d  %3d  %-6s  %-5s  any->any  %s  %s\n",
                 i, rules[i].priority,
                 rules[i].action == RULE_ALLOW ? "ALLOW" : "DENY",
                 rules[i].proto == PROTO_TCP ? "TCP" :
                 rules[i].proto == PROTO_UDP ? "UDP" : "ANY",
                 rules[i].direction == 0 ? "in " : rules[i].direction == 1 ? "out" : "any",
                 rules[i].comment);
        vga_print(buf);
    }
}

int shield_eval(uint32_t src_ip, uint32_t dst_ip,
                uint16_t src_port, uint16_t dst_port,
                rule_proto_t proto, int direction) {
    /* Evaluate rules in priority order (simple linear scan) */
    /* Find lowest-priority matching rule */
    int best_pri = 0x7fffffff;
    int best_act = RULE_ALLOW; /* default allow if no rule matches */

    for (int i = 0; i < rule_count; i++) {
        shield_rule_t *r = &rules[i];
        if (!r->active) continue;
        if (r->direction != 2 && r->direction != direction) continue;
        if (r->proto != PROTO_ANY && r->proto != proto) continue;
        if (!ip_match(src_ip, r->src_ip, r->src_mask)) continue;
        if (!ip_match(dst_ip, r->dst_ip, r->dst_mask)) continue;
        if (r->dst_port && r->dst_port != dst_port) continue;
        if (r->src_port && r->src_port != src_port) continue;

        if (r->priority < best_pri) {
            best_pri = r->priority;
            best_act = r->action;
        }
    }
    return (best_act == RULE_ALLOW) ? 1 : 0;
}

/* ---- Persistence via RAMFS -----------------------------------------------
 * Full block-device storage is a V2 item, but we can already persist the
 * rule table to /ramfs/shield.rules so that rules survive logout/login
 * within the same boot session.  The format is a flat binary dump of the
 * rules[] array (SHIELD_MAX_RULES × sizeof(shield_rule_t)).
 * -------------------------------------------------------------------- */

extern vfs_node_t *ramfs_root;   /* defined in kernel/init.c          */

#define SHIELD_RULES_FILE "shield.rules"

void shield_save(void) {
    if (!ramfs_root) {
        kwarn("SHIELD: save skipped — ramfs not mounted\n");
        return;
    }
    /* Create or overwrite the rules file */
    ramfs_create(ramfs_root, SHIELD_RULES_FILE);
    vfs_node_t *n = vfs_finddir(ramfs_root, SHIELD_RULES_FILE);
    if (!n) {
        kerror("SHIELD: save failed — could not create %s\n", SHIELD_RULES_FILE);
        return;
    }
    /* Write rule_count first (4 bytes), then the rule array */
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(rule_count & 0xFF);
    hdr[1] = (uint8_t)((rule_count >> 8) & 0xFF);
    hdr[2] = 0; hdr[3] = 0;
    vfs_write(n, 0, 4, hdr);
    vfs_write(n, 4, (uint32_t)(rule_count * (int)sizeof(shield_rule_t)),
              (const uint8_t *)rules);
    kinfo("SHIELD: saved %d rules to /ramfs/%s\n", rule_count, SHIELD_RULES_FILE);
}

void shield_load(void) {
    if (!ramfs_root) return;
    vfs_node_t *n = vfs_finddir(ramfs_root, SHIELD_RULES_FILE);
    if (!n) {
        kinfo("SHIELD: no saved rules found (/ramfs/%s absent) — using defaults\n",
              SHIELD_RULES_FILE);
        return;
    }
    uint8_t hdr[4] = {0};
    int r = vfs_read(n, 0, 4, hdr);
    if (r < 4) {
        kwarn("SHIELD: rules file too short — ignoring\n");
        return;
    }
    int saved_count = (int)hdr[0] | ((int)hdr[1] << 8);
    if (saved_count < 0 || saved_count > SHIELD_MAX_RULES) {
        kwarn("SHIELD: corrupt rules file (count=%d) — ignoring\n", saved_count);
        return;
    }
    int bytes = vfs_read(n, 4,
                         (uint32_t)(saved_count * (int)sizeof(shield_rule_t)),
                         (uint8_t *)rules);
    if (bytes > 0) {
        rule_count = saved_count;
        kinfo("SHIELD: loaded %d rules from /ramfs/%s\n",
              rule_count, SHIELD_RULES_FILE);
    }
}
