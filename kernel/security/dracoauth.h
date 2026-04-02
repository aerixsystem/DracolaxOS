/* kernel/security/dracoauth.h — DracoAuth: user account & permission system */
#ifndef DRACOAUTH_H
#define DRACOAUTH_H

#include "../types.h"

#define AUTH_MAX_USERS      8
#define AUTH_NAME_MAX       32
#define AUTH_PASS_MAX       64
#define AUTH_HASH_LEN       8   /* 32-bit CRC32 stored as hex string */

/* User roles */
#define ROLE_GUEST          0   /* Read-only, no install                */
#define ROLE_USER           1   /* Standard: apps, own files            */
#define ROLE_ADMIN          2   /* Full control, system files, packages */

/* Permission bits (per-object) */
#define PERM_READ           0x01
#define PERM_WRITE          0x02
#define PERM_EXEC           0x04
#define PERM_ADMIN_ONLY     0x08   /* Only admins may access             */

typedef struct {
    char     name[AUTH_NAME_MAX];
    char     pass_hash[12];       /* CRC32 hex + null                   */
    uint8_t  role;                /* ROLE_* constant                    */
    uint8_t  active;              /* 1 = account exists                 */
    uint32_t uid;
} draco_user_t;

/* Global logged-in state */
typedef struct {
    int           logged_in;     /* 1 if a session is active            */
    draco_user_t *user;          /* pointer into user table             */
} draco_session_t;

extern draco_session_t g_session;

/* Initialise auth subsystem (loads default admin + user accounts) */
void dracoauth_init(void);

/* Register a new account; returns 0 on success */
int  dracoauth_add_user(const char *name, const char *password, uint8_t role);

/* Attempt login; returns 0 on success and sets g_session */
int  dracoauth_login(const char *name, const char *password);

/* Log out current session */
void dracoauth_logout(void);

/* Check if current session has at least the given role */
int  dracoauth_has_role(uint8_t required_role);

/* Check permission bits on a path (simplified: just role check for now) */
int  dracoauth_check_perm(const char *path, uint8_t perm);

/* Return current user name (or "guest") */
const char *dracoauth_whoami(void);

/* List all users into buf */
void dracoauth_list_users(char *buf, size_t sz);

/* Change password for current user */
int  dracoauth_change_password(const char *old_pass, const char *new_pass);

#endif /* DRACOAUTH_H */
