/* kernel/security/dracolock.h — DracoLock: context-aware access control */
#ifndef DRACOLOCK_H
#define DRACOLOCK_H

#include "../types.h"

/* Context flags — determine access level */
#define CTX_TRUSTED_LOCAL    0   /* Local login, trusted hardware: full access  */
#define CTX_UNTRUSTED        1   /* Public/unknown: limited access              */
#define CTX_SUSPENDED        2   /* System locked / screensaver                 */
#define CTX_EMERGENCY        3   /* Hardware mismatch / critical error          */

typedef struct {
    uint8_t  context;            /* CTX_* constant                             */
    uint8_t  network_trusted;    /* 1 if on a trusted network                  */
    uint8_t  hw_verified;        /* 1 if hardware fingerprint matched           */
    uint8_t  screen_locked;      /* 1 if screen is locked                       */
    uint32_t lock_timeout;       /* ticks until auto-lock (0 = disabled)        */
    uint32_t last_activity;      /* pit_ticks() of last user input              */
} dracolock_state_t;

extern dracolock_state_t g_lock;

/* Initialise DracoLock */
void dracolock_init(void);

/* Evaluate current context and update g_lock.context */
void dracolock_update_context(void);

/* Check if access to a path is permitted under current context */
int  dracolock_can_access(const char *path, uint8_t perm);

/* Lock the screen */
void dracolock_lock_screen(void);

/* Unlock with password; returns 0 on success */
int  dracolock_unlock(const char *password);

/* Record activity (resets auto-lock timer) */
void dracolock_activity(void);

/* Set auto-lock timeout in seconds (0 = disabled) */
void dracolock_set_timeout(uint32_t seconds);

#endif /* DRACOLOCK_H */
