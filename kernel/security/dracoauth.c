/* kernel/security/dracoauth.c — DracoAuth implementation */
#include "../types.h"
#include "../klibc.h"
#include "../log.h"
#include "dracoauth.h"

/* ---- FNV-1a password hashing ------------------------------------------- */
/* FNV-1a 32-bit: fast, table-free, deterministic.
 * Replaces the truncated CRC32 table that had ~80 zero entries, causing
 * collisions and non-deterministic results.                                 */

static uint32_t fnv1a_32(const char *data, size_t len) {
    uint32_t hash = 0x811c9dc5u;   /* FNV offset basis */
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)data[i];
        hash *= 0x01000193u;       /* FNV prime */
    }
    return hash;
}

static void make_hash(const char *password, char out[12]) {
    /* hash = FNV-1a(password + "dracolax_salt") */
    char salted[AUTH_PASS_MAX + 16];
    strncpy(salted, password, AUTH_PASS_MAX - 1);
    salted[AUTH_PASS_MAX - 1] = '\0';
    strcat(salted, "dracolax_salt");
    uint32_t h = fnv1a_32(salted, strlen(salted));
    /* Store as 8 hex digits */
    const char *hex = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        out[i] = hex[h & 0xF];
        h >>= 4;
    }
    out[8] = '\0';
}

/* ---- user table --------------------------------------------------------- */

static draco_user_t users[AUTH_MAX_USERS];
static int          nusers = 0;

draco_session_t g_session = { .logged_in = 0, .user = NULL };

/* ---- public API --------------------------------------------------------- */

void dracoauth_init(void) {
    memset(users, 0, sizeof(users));
    nusers = 0;

    /* Built-in system accounts */
    dracoauth_add_user("root",  "dracolax",   ROLE_ADMIN);
    dracoauth_add_user("guest", "",           ROLE_GUEST);

    kinfo("AUTH: DracoAuth initialised (%d accounts)\n", nusers);
}

int dracoauth_add_user(const char *name, const char *password, uint8_t role) {
    if (nusers >= AUTH_MAX_USERS) return -1;
    if (!name || strlen(name) == 0) return -1;

    /* Check for duplicates */
    for (int i = 0; i < nusers; i++)
        if (strcmp(users[i].name, name) == 0) return -1;

    draco_user_t *u = &users[nusers];
    strncpy(u->name, name, AUTH_NAME_MAX - 1);
    make_hash(password, u->pass_hash);
    u->role   = role;
    u->active = 1;
    u->uid    = (uint32_t)(1000 + nusers);
    nusers++;
    return 0;
}

int dracoauth_login(const char *name, const char *password) {
    char hash[12];
    make_hash(password, hash);

    for (int i = 0; i < nusers; i++) {
        if (!users[i].active) continue;
        if (strcmp(users[i].name, name) == 0 &&
            strcmp(users[i].pass_hash, hash) == 0) {
            g_session.logged_in = 1;
            g_session.user      = &users[i];
            return 0;
        }
    }
    return -1; /* bad credentials */
}

void dracoauth_logout(void) {
    g_session.logged_in = 0;
    g_session.user      = NULL;
}

int dracoauth_has_role(uint8_t required_role) {
    if (!g_session.logged_in || !g_session.user) return 0;
    return g_session.user->role >= required_role;
}

int dracoauth_check_perm(const char *path, uint8_t perm) {
    (void)path;
    /* Admin: full access; User: no admin-only; Guest: read only */
    if (!g_session.logged_in) return 0;
    if (g_session.user->role == ROLE_ADMIN) return 1;
    if (perm & PERM_ADMIN_ONLY) return 0;
    if (g_session.user->role == ROLE_GUEST && (perm & PERM_WRITE)) return 0;
    return 1;
}

const char *dracoauth_whoami(void) {
    if (!g_session.logged_in || !g_session.user) return "guest";
    return g_session.user->name;
}

void dracoauth_list_users(char *buf, size_t sz) {
    size_t pos = 0;
    const char *roles[] = { "guest", "user", "admin" };
    for (int i = 0; i < nusers; i++) {
        if (!users[i].active) continue;
        const char *role = (users[i].role <= ROLE_ADMIN)
                           ? roles[users[i].role] : "?";
        int n = snprintf(buf + pos, sz - pos, "  uid=%-4u  %-16s  [%s]\n",
                         users[i].uid, users[i].name, role);
        if (n > 0) pos += (size_t)n;
        if (pos >= sz - 1) break;
    }
}

int dracoauth_change_password(const char *old_pass, const char *new_pass) {
    if (!g_session.logged_in || !g_session.user) return -1;
    char hash[12];
    make_hash(old_pass, hash);
    if (strcmp(g_session.user->pass_hash, hash) != 0) return -1;
    make_hash(new_pass, g_session.user->pass_hash);
    return 0;
}
