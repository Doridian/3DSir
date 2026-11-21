// 3DSir: start-up, settings, shutdown. The hardware is in irsend.c, the UI in shell.c and screens.c.

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app.h"
#include "log.h"

#define LOG_PATH "sdmc:/3dsir.log" // debug build only

char app_path[256] = "sdmc:/3ds/3dsir.3dsx";
volatile bool restart_requested;

IrCarrierStyle style = IrCarrierDense;
unsigned repeats = 3;

static void settings_load(void) {
    FILE* f = fopen(SETTINGS_PATH, "r");
    if(!f) return;
    char line[64];
    while(fgets(line, sizeof(line), f)) {
        unsigned v;
        if(!strncmp(line, "style=clean", 11)) style = IrCarrierClean;
        if(!strncmp(line, "style=dense", 11)) style = IrCarrierDense;
        if(sscanf(line, "repeats=%u", &v) == 1 && v >= 1 && v <= 10) repeats = v;
    }
    fclose(f);
}

void settings_save(void) {
    mkdir(ROOT_DIR, 0777);
    FILE* f = fopen(SETTINGS_PATH, "w");
    if(!f) return;
    fprintf(f, "style=%s\nrepeats=%u\n", style == IrCarrierClean ? "clean" : "dense", repeats);
    fclose(f);
}

// Relaunch the (updated) .3dsx the way the Homebrew Menu starts one under Luma: tell Rosalina's
// hb:ldr the target and its argv, then soft-reset into it. Without hb:ldr this is a plain exit.
static void relaunch_self(void) {
    Handle hbldr;
    if(R_FAILED(svcConnectToPort(&hbldr, "hb:ldr"))) return;
    const char* path = strncmp(app_path, "sdmc:/", 6) ? app_path : app_path + 5; // SD-relative
    static u32 args[0x400 / 4]; // argc, then the NUL-separated strings
    args[0] = 1;
    strcpy((char*)(args + 1), app_path);
    u32* cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(2, 0, 2); // SetTarget
    cmd[1] = IPC_Desc_StaticBuffer(strlen(path) + 1, 0);
    cmd[2] = (u32)path;
    Result rc = svcSendSyncRequest(hbldr);
    if(R_SUCCEEDED(rc)) rc = cmd[1];
    if(R_SUCCEEDED(rc)) {
        cmd[0] = IPC_MakeHeader(3, 0, 2); // SetArgv
        cmd[1] = IPC_Desc_StaticBuffer(sizeof(args), 1);
        cmd[2] = (u32)args;
        rc = svcSendSyncRequest(hbldr);
        if(R_SUCCEEDED(rc)) rc = cmd[1];
    }
    svcCloseHandle(hbldr);
    if(R_SUCCEEDED(rc)) aptSetChainloaderToSelf();
}

int main(int argc, char* argv[]) {
    if(argc > 0 && argv[0] && !strncmp(argv[0], "sdmc:/", 6)) snprintf(app_path, sizeof(app_path), "%s", argv[0]);
    gfxInitDefault();
    ui_init();
    LightLock_Init(&hw_lock);
#ifdef APP_RELEASE
    log_init(NULL);
    logp("3DSir (built " __DATE__ " " __TIME__ ")\n");
#else
    log_init(LOG_PATH);
    logp("--- 3DSir debug (built " __DATE__ " " __TIME__ ") ---\n");
    logp("app: %s\n", app_path);
    net_start();
#endif
    mkdir(ROOT_DIR, 0777);
    settings_load();
    if(R_FAILED(iru_open())) logp("ir:u unavailable\n");

    app_run(screens_root());

#ifndef APP_RELEASE
    net_stop();
#endif
    // A net client stuck in a hardware call holds hw_lock forever: don't wait on it to exit.
    bool locked = false;
    for(int i = 0; i < 20 && !(locked = !LightLock_TryLock(&hw_lock)); i++) svcSleepThread(50000000LL);
    if(locked) {
        if(!iru_is_open()) app_hw_iru(); // a net client left the hardware taken over
        iru_close();
        LightLock_Unlock(&hw_lock);
    } else {
        logp("hw_lock held by a stuck call, exiting without giving the hardware back\n");
    }
    logp("--- exit ---\n");
    log_exit();

    ui_exit();
    gfxExit();
    if(restart_requested) relaunch_self();
    return 0;
}
