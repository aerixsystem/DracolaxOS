/* services/service_manager.h — DracolaxOS system service registry
 *
 * All services are kernel tasks registered here and started by init.
 * Each service implements the service_t interface.
 */
#ifndef SERVICE_MANAGER_H
#define SERVICE_MANAGER_H
#include "../kernel/types.h"

typedef enum {
    SVC_STATE_STOPPED = 0,
    SVC_STATE_STARTING,
    SVC_STATE_RUNNING,
    SVC_STATE_FAILED,
} svc_state_t;

typedef struct {
    const char  *name;
    void       (*task_fn)(void);   /* kernel task entry point */
    svc_state_t  state;
    int          restart_on_fail;
} service_t;

void svc_manager_init(void);
void svc_manager_start_all(void);
int  svc_start(const char *name);
int  svc_stop(const char *name);
svc_state_t svc_status(const char *name);

#endif
