#include "../../types.h"
#include "../../klibc.h"
#include "input_driver.h"
#define QSZ 64
static input_event_t q[QSZ];
static volatile int qh=0,qt=0;
void input_push(const input_event_t *ev){int n=(qt+1)%QSZ;if(n!=qh){q[qt]=*ev;qt=n;}}
int  input_poll(input_event_t *out){if(qh==qt)return 0;*out=q[qh];qh=(qh+1)%QSZ;return 1;}
