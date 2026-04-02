/* lxscript/string.h — kernel-compat shim
 * Redirects <string.h> to kernel/klibc.h when built freestanding (-nostdinc).
 * The kernel's klibc provides: memcpy, memset, memcmp, strlen, strcmp,
 * strncmp, strncpy, strcpy, strcat, strchr, strrchr, snprintf, etc.
 */
#pragma once
#include "../kernel/klibc.h"
