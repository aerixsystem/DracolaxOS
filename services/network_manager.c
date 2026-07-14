/* services/network_manager.c — DracolaxOS network manager service
 *
 * Responsibilities:
 *   - Poll network driver for link state changes
 *   - Manage interface up/down lifecycle
 *   - Provide simple dhcp-like address assignment stub
 *
 * Interface: net_manager_task() runs as a kernel sched task.
 */
#include "../kernel/types.h"
#include "../kernel/log.h"
#include "../kernel/sched/sched.h"
#include "../kernel/klibc.h"
#include "network_manager.h"

static net_iface_t ifaces[NET_MAX_IFACES];
static int iface_count = 0;

void net_manager_init(void) {
    memset(ifaces, 0, sizeof(ifaces));
    iface_count = 0;
    kinfo("NET-MGR: initialised\n");
}

int net_iface_register(const char *name, net_send_fn send, net_recv_fn recv) {
    if (iface_count >= NET_MAX_IFACES) return -1;
    net_iface_t *iface = &ifaces[iface_count++];
    strncpy(iface->name, name, sizeof(iface->name) - 1);
    iface->send = send;
    iface->recv = recv;
    iface->state = NET_IFACE_DOWN;
    kinfo("NET-MGR: registered iface %s\n", name);
    return 0;
}

int net_iface_up(const char *name) {
    for (int i = 0; i < iface_count; i++) {
        if (strcmp(ifaces[i].name, name) == 0) {
            ifaces[i].state = NET_IFACE_UP;
            kinfo("NET-MGR: %s up\n", name);
            return 0;
        }
    }
    return -1;
}

int net_iface_down(const char *name) {
    for (int i = 0; i < iface_count; i++) {
        if (strcmp(ifaces[i].name, name) == 0) {
            ifaces[i].state = NET_IFACE_DOWN;
            kinfo("NET-MGR: %s down\n", name);
            return 0;
        }
    }
    return -1;
}

void net_manager_task(void) {
    net_manager_init();
    kinfo("NET-MGR: task running\n");
    for (;;) {
        /* Poll all interfaces for incoming packets */
        for (int i = 0; i < iface_count; i++) {
            if (ifaces[i].state == NET_IFACE_UP && ifaces[i].recv) {
                uint8_t buf[1518];
                int len = ifaces[i].recv(buf, sizeof(buf));
                if (len > 0) {
                    /* Dispatch to protocol stack (stub) */
                    kinfo("NET-MGR: rx %d bytes on %s\n", len, ifaces[i].name);
                }
            }
        }
        sched_sleep(100);
    }
}
