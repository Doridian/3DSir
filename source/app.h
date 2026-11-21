#pragma once

#include <3ds.h>
#include <stdbool.h>

#include "irenc.h"

#define ROOT_DIR "sdmc:/3ds/3dsir"

// Held by whoever touches the IR hardware: the UI's sends and the net server's requests.
extern LightLock hw_lock;

extern char app_path[256]; // the running .3dsx (argv[0])
extern volatile bool restart_requested; // exit the UI and relaunch app_path (net self-update)

// ir:u path. app_send fails while the hardware is taken over.
Result app_send(const IrTimings* tm, IrCarrierStyle style, u32* bytes);
// Raw bytes through ir:u at one IRU_SetBitRate value; *us = time until on air.
Result app_iru_send(u8 bitrate, const u8* data, u32 len, u32* us);

// Direct hardware: stop ir:u + the ir module and take the SC16IS750 (hw_lock held).
Result app_hw_direct(u32 flags);
extern volatile bool trim_overridden; // the net server set ir_mark_trim_us: use it for every style
// Give the hardware back and reopen ir:u (hw_lock held).
void app_hw_iru(void);

#ifndef APP_RELEASE
// netsrv.c
#define NET_PORT 8020
extern char net_addr[32];
extern volatile u32 net_keys; // KEY_* pressed remotely, consumed by the UI's next input scan
void net_start(void);
void net_stop(void);
#endif
