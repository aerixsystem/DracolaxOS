#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H
#include "../kernel/types.h"

#define NET_MAX_IFACES 4

typedef int (*net_send_fn)(const uint8_t *buf, int len);
typedef int (*net_recv_fn)(uint8_t *buf, int maxlen);

typedef enum { NET_IFACE_DOWN=0, NET_IFACE_UP=1 } net_iface_state_t;

typedef struct {
    char             name[16];
    net_send_fn      send;
    net_recv_fn      recv;
    net_iface_state_t state;
    uint8_t          mac[6];
    uint32_t         ip4;
} net_iface_t;

void net_manager_init(void);
int  net_iface_register(const char *name, net_send_fn send, net_recv_fn recv);
int  net_iface_up(const char *name);
int  net_iface_down(const char *name);
void net_manager_task(void);

#endif
