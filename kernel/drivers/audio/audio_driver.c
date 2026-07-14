#include "../../types.h"
#include "../../log.h"
#include "audio_driver.h"
static audio_driver_t *active=NULL;
void audio_driver_register(audio_driver_t *d){
    if(d&&d->init&&d->init()==0){active=d;kinfo("AUDIO-DRV: '%s'\n",d->name);}
}
