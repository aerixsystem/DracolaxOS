/* draco-shield/firewall.h — Draco Shield packet filter */
#ifndef FIREWALL_H
#define FIREWALL_H

#include "../../types.h"

#define SHIELD_MAX_RULES  64
#define SHIELD_IFACE_LEN  16
#define SHIELD_COMMENT_LEN 64

typedef enum {
    RULE_ALLOW = 0,
    RULE_DENY  = 1,
} rule_action_t;

typedef enum {
    PROTO_ANY = 0,
    PROTO_TCP = 6,
    PROTO_UDP = 17,
    PROTO_ICMP = 1,
} rule_proto_t;

typedef struct {
    int           active;
    int           priority;        /* lower = evaluated first          */
    rule_action_t action;
    rule_proto_t  proto;
    uint32_t      src_ip;          /* 0 = any                          */
    uint32_t      src_mask;
    uint32_t      dst_ip;          /* 0 = any                          */
    uint32_t      dst_mask;
    uint16_t      dst_port;        /* 0 = any                          */
    uint16_t      src_port;        /* 0 = any                          */
    char          iface[SHIELD_IFACE_LEN];   /* "" = any               */
    int           direction;       /* 0=in 1=out 2=any                 */
    int           stateful;        /* 1 = allow established if matched */
    char          comment[SHIELD_COMMENT_LEN];
} shield_rule_t;

/* Initialise with default rules */
void shield_init(void);

/* Add a rule; returns rule index or -1 */
int  shield_add_rule(const shield_rule_t *rule);

/* Remove a rule by index */
int  shield_remove_rule(int idx);

/* List all active rules */
void shield_list_rules(void);

/* Evaluate a packet. Returns 1 if allowed, 0 if denied. */
int  shield_eval(uint32_t src_ip, uint32_t dst_ip,
                 uint16_t src_port, uint16_t dst_port,
                 rule_proto_t proto, int direction);

/* Persist rules to /storage/main/system/security/rules.db */
void shield_save(void);
void shield_load(void);

#endif /* FIREWALL_H */
