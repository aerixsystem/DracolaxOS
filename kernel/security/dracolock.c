/* kernel/security/dracolock.c */
#include "../types.h"
#include "../klibc.h"
#include "../log.h"
#include "../arch/x86_64/pic.h"
#include "dracolock.h"
#include "dracoauth.h"
#include "dracolicence.h"

dracolock_state_t g_lock = {
    .context        = CTX_TRUSTED_LOCAL,
    .network_trusted = 0,
    .hw_verified    = 0,
    .screen_locked  = 0,
    .lock_timeout   = 0,
    .last_activity  = 0,
};

void dracolock_init(void) {
    g_lock.hw_verified = (uint8_t)dracolicence_verify();
    g_lock.last_activity = pit_ticks();
    dracolock_update_context();
    kinfo("LOCK: DracoLock init — context=%u hw_verified=%u\n",
          g_lock.context, g_lock.hw_verified);
}

void dracolock_update_context(void) {
    if (!g_lock.hw_verified) {
        g_lock.context = CTX_EMERGENCY;
        return;
    }
    if (g_lock.screen_locked) {
        g_lock.context = CTX_SUSPENDED;
        return;
    }
    if (!g_lock.network_trusted) {
        g_lock.context = CTX_TRUSTED_LOCAL; /* No network yet; default to trusted */
    } else {
        g_lock.context = CTX_TRUSTED_LOCAL;
    }
}

int dracolock_can_access(const char *path, uint8_t perm) {
    (void)perm;
    switch (g_lock.context) {
    case CTX_TRUSTED_LOCAL:
        return 1; /* Full access */
    case CTX_UNTRUSTED:
        /* Block /storage/main/system writes in untrusted context */
        if (strncmp(path, "/storage/main/system", 20) == 0 &&
            (perm & PERM_WRITE)) return 0;
        return 1;
    case CTX_SUSPENDED:
    case CTX_EMERGENCY:
        return 0; /* Block everything */
    default:
        return 1;
    }
}

void dracolock_lock_screen(void) {
    g_lock.screen_locked = 1;
    dracolock_update_context();
    kinfo("LOCK: screen locked\n");
}

int dracolock_unlock(const char *password) {
    if (!g_session.logged_in || !g_session.user) return -1;
    if (dracoauth_login(g_session.user->name, password) == 0) {
        g_lock.screen_locked = 0;
        g_lock.last_activity = pit_ticks();
        dracolock_update_context();
        return 0;
    }
    return -1;
}

void dracolock_activity(void) {
    g_lock.last_activity = pit_ticks();
    if (g_lock.screen_locked) return; /* Don't auto-unlock on activity */
}

void dracolock_set_timeout(uint32_t seconds) {
    g_lock.lock_timeout = seconds * 100; /* Convert to ticks at 100Hz */
}
