/* kernel/security/dracolicence.c */
#include "../types.h"
#include "../klibc.h"
#include "../log.h"
#include "../mm/pmm.h"
#include "dracolicence.h"

draco_licence_t g_licence = { .valid = 0 };

/* Read CPUID for hardware fingerprint; fallback to PMM state */
static uint32_t hw_fingerprint(void) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    /* CPUID leaf 1 — processor signature */
    __asm__ volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1)
    );
    /* Mix eax (stepping/model/family) with edx (feature flags) */
    uint32_t fp = (eax ^ edx ^ (ebx << 4) ^ (ecx >> 2));
    /* Mix with free page count as extra entropy */
    fp ^= (pmm_free_pages() * 0x9E3779B9u);
    return fp;
}

static void u32_to_hex4(uint32_t v, char *out) {
    const char *h = "0123456789ABCDEF";
    out[0] = h[(v >> 12) & 0xF];
    out[1] = h[(v >>  8) & 0xF];
    out[2] = h[(v >>  4) & 0xF];
    out[3] = h[(v      ) & 0xF];
}

void dracolicence_gen_device_id(char out[DEVICE_ID_LEN]) {
    uint32_t fp   = hw_fingerprint();
    uint32_t fp2  = fp ^ 0xDEADBEEF;
    uint32_t fp3  = fp * 0x6C62272Eu;
    uint32_t fp4  = fp2 ^ (fp3 >> 5);

    /* Format: DX-XXXX-XXXX-XXXX-XXXX */
    out[0]  = 'D'; out[1] = 'X'; out[2] = '-';
    u32_to_hex4(fp,  out + 3);  out[7]  = '-';
    u32_to_hex4(fp2, out + 8);  out[12] = '-';
    u32_to_hex4(fp3, out + 13); out[17] = '-';
    u32_to_hex4(fp4, out + 18); out[22] = '\0';
}

void dracolicence_activate(void) {
    dracolicence_gen_device_id(g_licence.device_id);

    /* Licence ID = "DXOS-" + first 16 chars of device_id scrambled */
    uint32_t seed = 0;
    for (int i = 0; i < (int)strlen(g_licence.device_id); i++)
        seed = seed * 31 + (uint8_t)g_licence.device_id[i];

    uint32_t a = seed ^ 0xC0FFEEBA;
    uint32_t b = seed * 0x1234567u;
    uint32_t c = a ^ (b >> 4);
    uint32_t d = b ^ (a << 3);

    g_licence.licence_id[0]  = 'D'; g_licence.licence_id[1]  = 'X';
    g_licence.licence_id[2]  = 'O'; g_licence.licence_id[3]  = 'S';
    g_licence.licence_id[4]  = '-';
    u32_to_hex4(a, g_licence.licence_id + 5);  g_licence.licence_id[9]  = '-';
    u32_to_hex4(b, g_licence.licence_id + 10); g_licence.licence_id[14] = '-';
    u32_to_hex4(c, g_licence.licence_id + 15); g_licence.licence_id[19] = '-';
    u32_to_hex4(d, g_licence.licence_id + 20); g_licence.licence_id[24] = '\0';

    g_licence.valid      = 1;
    g_licence.generation = 1;
}

int dracolicence_verify(void) {
    if (!g_licence.valid) return 0;
    /* Verify device ID matches current hardware */
    char current[DEVICE_ID_LEN];
    dracolicence_gen_device_id(current);
    return (strcmp(current, g_licence.device_id) == 0) ? 1 : 0;
}

void dracolicence_init(void) {
    dracolicence_activate();
    int ok = dracolicence_verify();
    kinfo("LICENCE: Device ID: %s\n", g_licence.device_id);
    kinfo("LICENCE: Licence ID: %s (%s)\n", g_licence.licence_id,
          ok ? "verified" : "MISMATCH");
}

const char *dracolicence_device_id(void) { return g_licence.device_id; }
const char *dracolicence_licence_id(void) { return g_licence.licence_id; }
