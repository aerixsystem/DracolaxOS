#ifndef NET_DRIVER_H
#define NET_DRIVER_H
#include "../../types.h"
typedef struct net_driver {
    const char *name;
    int  (*init)(void);
    int  (*send)(const uint8_t *buf, int len);
    int  (*recv)(uint8_t *buf, int max);
    void (*get_mac)(uint8_t mac[6]);
} net_driver_t;
void net_driver_register(net_driver_t *d);
#endif
