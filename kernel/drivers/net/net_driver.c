#include "../../types.h"
#include "../../log.h"
#include "net_driver.h"
static net_driver_t *active=NULL;
void net_driver_register(net_driver_t *d){
    if(d&&d->init&&d->init()==0){active=d;kinfo("NET-DRV: '%s'\n",d->name);}
}
