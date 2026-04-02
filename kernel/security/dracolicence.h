/* kernel/security/dracolicence.h */
#ifndef DRACOLICENCE_H
#define DRACOLICENCE_H

#include "../types.h"

#define LICENCE_ID_LEN  32  /* "DXOS-XXXX-XXXX-XXXX-XXXX" format */
#define DEVICE_ID_LEN   24  /* "DX-XXXXXXXXXXXXXXXX"              */

typedef struct {
    char     device_id[DEVICE_ID_LEN];
    char     licence_id[LICENCE_ID_LEN];
    uint8_t  valid;         /* 1 = licence bound and verified  */
    uint32_t generation;    /* hardware generation counter     */
} draco_licence_t;

extern draco_licence_t g_licence;

/* Initialise: generate device ID from hardware fingerprint */
void dracolicence_init(void);

/* Generate device ID string */
void dracolicence_gen_device_id(char out[DEVICE_ID_LEN]);

/* Bind and activate licence */
void dracolicence_activate(void);

/* Verify current hardware matches stored device ID */
int  dracolicence_verify(void);

/* Get device ID string */
const char *dracolicence_device_id(void);

/* Get licence ID string */
const char *dracolicence_licence_id(void);

#endif /* DRACOLICENCE_H */
