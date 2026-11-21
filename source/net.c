// soc:U, shared: the debug build's TCP server and the remote database download both need it, and socInit
// can't be called twice.

#include <3ds.h>
#include <malloc.h>

#include "app.h"

#define SOC_BUFFERSIZE 0x100000

static u32* soc_buf;
static int soc_refs;
static LightLock soc_lock = 1; // unlocked

bool soc_acquire(void) {
    LightLock_Lock(&soc_lock);
    bool ok = true;
    if(!soc_refs) {
        soc_buf = memalign(0x1000, SOC_BUFFERSIZE);
        ok = soc_buf && R_SUCCEEDED(socInit(soc_buf, SOC_BUFFERSIZE));
        if(!ok) {
            free(soc_buf);
            soc_buf = NULL;
        }
    }
    if(ok) soc_refs++;
    LightLock_Unlock(&soc_lock);
    return ok;
}

void soc_release(void) {
    LightLock_Lock(&soc_lock);
    if(soc_refs && !--soc_refs) {
        socExit();
        free(soc_buf);
        soc_buf = NULL;
    }
    LightLock_Unlock(&soc_lock);
}
