/* services/service_manager.c — DracolaxOS service manager */
#include "../kernel/types.h"
#include "../kernel/log.h"
#include "../kernel/sched/sched.h"
#include "../kernel/klibc.h"
#include "service_manager.h"

/* Forward declarations for service task functions */
void net_manager_task(void);
void power_manager_task(void);
void audio_service_task(void);
void notification_daemon_task(void);
void session_manager_task(void);
void login_manager_task(void);

static service_t services[] = {
    { "net-manager",       net_manager_task,       SVC_STATE_STOPPED, 1 },
    { "power-manager",     power_manager_task,     SVC_STATE_STOPPED, 1 },
    { "audio-service",     audio_service_task,     SVC_STATE_STOPPED, 1 },
    { "notification-daemon", notification_daemon_task, SVC_STATE_STOPPED, 1 },
    { "session-manager",   session_manager_task,   SVC_STATE_STOPPED, 1 },
    { "login-manager",     login_manager_task,     SVC_STATE_STOPPED, 0 },
};
#define SVC_COUNT ((int)(sizeof(services)/sizeof(services[0])))

void svc_manager_init(void) {
    kinfo("SVC: service manager initialised (%d services)\n", SVC_COUNT);
}

void svc_manager_start_all(void) {
    for (int i = 0; i < SVC_COUNT; i++) {
        if (sched_spawn(services[i].task_fn, services[i].name) >= 0) {
            services[i].state = SVC_STATE_RUNNING;
            kinfo("SVC: started %s\n", services[i].name);
        } else {
            services[i].state = SVC_STATE_FAILED;
            kerror("SVC: failed to start %s\n", services[i].name);
        }
    }
}

int svc_start(const char *name) {
    for (int i = 0; i < SVC_COUNT; i++) {
        if (strcmp(services[i].name, name) == 0) {
            if (sched_spawn(services[i].task_fn, name) >= 0) {
                services[i].state = SVC_STATE_RUNNING;
                return 0;
            }
            services[i].state = SVC_STATE_FAILED;
            return -1;
        }
    }
    return -1;
}

int svc_stop(const char *name) {
    for (int i = 0; i < SVC_COUNT; i++) {
        if (strcmp(services[i].name, name) == 0) {
            services[i].state = SVC_STATE_STOPPED;
            kinfo("SVC: %s stopped\n", name);
            return 0;
        }
    }
    return -1;
}

svc_state_t svc_status(const char *name) {
    for (int i = 0; i < SVC_COUNT; i++)
        if (strcmp(services[i].name, name) == 0) return services[i].state;
    return SVC_STATE_STOPPED;
}
