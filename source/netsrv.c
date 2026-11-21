// Debug build only: a TCP server so the PC side (tools/irnet.py) can run IR experiments,
// send codes, update the app and drive its UI without rebuilding or touching the console.
// Same framing as Prox3DS. Up to three clients, each on its own thread.
//
// Request:  op u8, len u16 LE, payload
// Response: op u8, result u32 LE, len u16 LE, payload

#include <arpa/inet.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "irfile.h"
#include "irhw.h"
#include "log.h"

#define SOC_BUFFERSIZE 0x100000
#define MS(x) ((s64)(x) * 1000000LL)

enum {
    OP_PING = 0x00, // -> "3DSir"
    OP_TAKEOVER = 0x01, // [flags u8, default 4: 4 steal the ir module's i2c::IR session (Luma's
                        // svcControlService) and save the registers; 1 kill the ir module instead;
                        // 2 open gpio:IR] close ir:u, IER = 0, transceiver/TX/RX/FIFOs on
    OP_RELEASE = 0x02, // after a steal: restore the registers, close our handle, reopen ir:u. After a
                       // kill this reboots: the killed ir never finishes dying
    OP_REG_READ = 0x03, // reg u8, count u8 -> count bytes of that register (RHR: FIFO bytes)
    OP_REG_WRITE = 0x04, // reg u8, bytes (THR: into the FIFO)
    OP_GPIO = 0x05, // value u32, mask u32 (0 = no write), read mask u32 -> u32 data
    OP_I2C = 0x06, // dev u8, read len u16, bytes: raw write (if any), then raw read (if len)
    OP_IPC = 0x07, // target u8 (0 i2c::IR, 1 gpio:IR), static out size u16, words u32... (header
                   // first) -> 16 reply words, then the static buffer
    OP_UPDATE = 0x08, // offset u32 (0 = start over), data: chunks of a new .3dsx to <app>.new
    OP_UPDATE_DONE = 0x09, // size u32: checks it, swaps it in, the app exits and relaunches itself
    OP_PUT_FILE = 0x0A, // offset u32 (0 = new file), path len u8, path (under sdmc:/3ds/3dsir/), data
    OP_BENCH = 0x0B, // n u16 -> min, avg, max u32: microseconds per single-register I2C read
    OP_TX = 0x0C, // bytes -> through the TX FIFO as fast as I2C allows; waits for the shifter to
                  // drain -> elapsed us u32, writes u32, times the FIFO was found empty u32, bytes sent u32
                  // (short when TXLVL stayed 0 for 2 s)
    OP_RX = 0x0D, // ms u16 -> polls RXLVL for that long and drains the FIFO: polls u32, elapsed us
                  // u32, samples u32, then per sample: t us u32, n u8, n bytes
    OP_SEND = 0x0E, // a code through ir:u. kind u8, style u8, repeats u8, then kind 0 (parsed):
                    // protocol char[16], address u32, command u32; kind 1 (raw): freq u32, count
                    // u16, count u32 timings -> bytes rendered u32
    OP_IRU_SEND = 0x0F, // bitrate value u8, bytes: raw through ir:u -> us u32 until on air
    OP_SET_TRIM = 0x10, // trim s32: ir_mark_trim_us for every later send, either style (by default
                        // clean gets 80, dense 0) -> the old value s32
    // UI debugging, served without hw_lock:
    OP_SHOT = 0x20, // offset u32 [, screen u8: 1 = bottom] -> up to OUT_MAX bytes of the framebuffer (240x400 or
                    // 240x320 BGR8, column-major from the bottom left), empty past the end
    OP_KEYS = 0x21, // keys u32 (KEY_*) -> pressed for one frame of the UI
    OP_TOUCH = 0x22, // x u16, y u16 -> a tap on the bottom screen, for one frame of the UI
    OP_LOG = 0x24, // lines u32 -> the last that many log lines, '\n' each
    OP_GET_FILE = 0x25, // offset u32, path (any sdmc:/ file) -> up to OUT_MAX bytes from there
};

#define OUT_MAX 0xF000

char net_addr[32];
volatile u32 net_keys;
volatile u32 net_touch;

static u32* soc_buf;
static int listen_fd = -1;
static Thread net_thread;
static volatile bool net_quit;

// Waits until fd is readable, in short steps, so net_stop never has to break a blocked socket
// call (that hangs soc:U, and the system with it).
static bool wait_readable(int fd) {
    while(!net_quit) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int r = poll(&pfd, 1, 100);
        if(r < 0) return false;
        if(r > 0) return true;
    }
    return false;
}

static bool recv_all(int fd, void* buf, u32 len) {
    u8* p = buf;
    while(len) {
        if(!wait_readable(fd)) return false;
        int n = recv(fd, p, len, 0);
        if(n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

static bool send_reply(int fd, u8 op, Result res, const void* payload, u16 len) {
    u8 hdr[7] = {op, res, res >> 8, res >> 16, res >> 24, len, len >> 8};
    if(send(fd, hdr, sizeof(hdr), 0) != sizeof(hdr)) return false;
    return !len || send(fd, payload, len, 0) == len;
}

static void put32(u8* p, u32 v) {
    memcpy(p, &v, 4);
}

static u32 get32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

static Result handle_debug(u8 op, const u8* in, u32 in_len, u8* out, u16* out_len) {
    if(in_len < 4) return -3;
    u32 v = get32(in);
    if(op == OP_GET_FILE) {
        char path[256];
        u32 k = in_len - 4 < sizeof(path) - 1 ? in_len - 4 : sizeof(path) - 1;
        memcpy(path, in + 4, k);
        path[k] = 0;
        FILE* f = fopen(path, "rb");
        if(!f) return -5;
        if(!fseek(f, v, SEEK_SET)) *out_len = fread(out, 1, OUT_MAX, f);
        fclose(f);
        return 0;
    }
    if(op == OP_KEYS) {
        __atomic_or_fetch(&net_keys, v, __ATOMIC_SEQ_CST);
        return 0;
    }
    if(op == OP_TOUCH) {
        __atomic_store_n(&net_touch, 0x80000000u | (v & 0x7FFF7FFF), __ATOMIC_SEQ_CST);
        return 0;
    }
    if(op == OP_LOG) {
        int n = log_count(), i = n - (int)v < 0 ? 0 : n - (int)v;
        char line[128];
        for(; i < n; i++) {
            log_line(i, line, sizeof(line));
            u32 k = strlen(line);
            if(*out_len + k + 1 > OUT_MAX) break;
            memcpy(out + *out_len, line, k);
            *out_len += k;
            out[(*out_len)++] = '\n';
        }
        return 0;
    }
    u16 w, h;
    gfxScreen_t scr = in_len > 4 && in[4] == 1 ? GFX_BOTTOM : GFX_TOP;
    u8* fb = gfxGetFramebuffer(scr, GFX_LEFT, &w, &h);
    u32 size = w * h * gspGetBytesPerPixel(gfxGetScreenFormat(scr));
    if(v < size) {
        *out_len = size - v < OUT_MAX ? size - v : OUT_MAX;
        GSPGPU_InvalidateDataCache(fb + v, *out_len);
        memcpy(out, fb + v, *out_len);
    }
    return 0;
}

static Result handle_update(u8 op, const u8* in, u32 in_len) {
    char tmp[sizeof(app_path) + 4];
    snprintf(tmp, sizeof(tmp), "%s.new", app_path);
    if(in_len < 4) return -3;
    u32 n = get32(in);
    if(op == OP_UPDATE) {
        FILE* f = fopen(tmp, n ? "r+b" : "wb");
        if(!f) return -5;
        bool ok = !fseek(f, n, SEEK_SET) && fwrite(in + 4, 1, in_len - 4, f) == in_len - 4;
        return fclose(f) || !ok ? -5 : 0;
    }
    struct stat st;
    if(stat(tmp, &st) || st.st_size != n) return -6;
    remove(app_path);
    if(rename(tmp, app_path)) return -5;
    logp("net: app updated (%lu bytes), restarting\n", n);
    restart_requested = true;
    return 0;
}

static Result handle_put_file(const u8* in, u32 in_len) {
    if(in_len < 5 || in_len < 5u + in[4]) return -3;
    u32 off = get32(in);
    char path[256];
    memcpy(path, in + 5, in[4]);
    path[in[4]] = 0;
    if(strncmp(path, ROOT_DIR "/", strlen(ROOT_DIR "/")) || strstr(path, "..")) return -3;
    for(char* p = path + strlen(ROOT_DIR "/"); (p = strchr(p, '/')); p++) {
        *p = 0;
        mkdir(path, 0777);
        *p = '/';
    }
    FILE* f = fopen(path, off ? "r+b" : "wb");
    if(!f) return -5;
    u32 n = in_len - 5 - in[4];
    bool ok = !fseek(f, off, SEEK_SET) && fwrite(in + 5 + in[4], 1, n, f) == n;
    return fclose(f) || !ok ? -5 : 0;
}

// Pushes bytes through the 64-byte TX FIFO, topping it up whenever TXLVL shows room.
static Result direct_tx(const u8* data, u32 n, u8* out, u16* out_len) {
    u32 sent = 0, writes = 0, empty = 0;
    u64 t0 = svcGetSystemTick(), last = t0;
    while(sent < n) {
        u8 room = 0;
        Result rc = sc_get(SC_TXLVL, &room);
        if(R_FAILED(rc)) return rc;
        if(room > 64) room = 64;
        if(!room) {
            // The FIFO stopped draining (transmitter disabled, or no clock): give up rather than
            // hold the hardware lock and the connection forever.
            if(svcGetSystemTick() - last > 2ULL * SYSCLOCK_ARM11) break;
            continue;
        }
        last = svcGetSystemTick();
        if(room == 64 && sent) empty++; // it ran dry: there was a gap on air
        u32 k = n - sent < room ? n - sent : room;
        rc = sc_write(SC_THR, data + sent, k);
        if(R_FAILED(rc)) return rc;
        sent += k;
        writes++;
    }
    // LSR[6]: THR and TSR empty, the last stop bit is out.
    // Bounded: with the transmitter disabled (EFCR[2], how the ir module leaves it) it never drains.
    bool drained = false;
    for(int i = 0; i < 1000 && !drained; i++) {
        u8 lsr = 0;
        if(R_FAILED(sc_get(SC_LSR, &lsr))) break;
        drained = lsr & 0x40;
    }
    if(!drained) sent |= 0x80000000;
    put32(out, irhw_us(svcGetSystemTick() - t0));
    put32(out + 4, writes);
    put32(out + 8, empty);
    put32(out + 12, sent);
    *out_len = 16;
    return 0;
}

// Polls RXLVL and drains the RX FIFO for ms milliseconds: whether, and how finely, received
// light can be timed from here.
static Result direct_rx(u32 ms, u8* out, u16* out_len) {
    u32 polls = 0, samples = 0, o = 12;
    u64 t0 = svcGetSystemTick(), end = t0 + (u64)ms * SYSCLOCK_ARM11 / 1000;
    // Flush first: the transceiver hears its own TX, which fills the FIFO and drops what follows.
    Result rc = sc_set(SC_FCR, 0x03);
    if(R_FAILED(rc)) return rc;
    for(;;) {
        u64 now = svcGetSystemTick();
        if(now >= end) break;
        u8 lvl = 0;
        rc = sc_get(SC_RXLVL, &lvl);
        if(R_FAILED(rc)) break;
        polls++;
        if(!lvl) continue;
        if(lvl > 64) lvl = 64;
        if(o + 5 + lvl > OUT_MAX) break;
        put32(out + o, irhw_us(now - t0));
        out[o + 4] = lvl;
        rc = sc_read(SC_RHR, out + o + 5, lvl);
        if(R_FAILED(rc)) break;
        o += 5 + lvl;
        samples++;
    }
    put32(out, polls);
    put32(out + 4, irhw_us(svcGetSystemTick() - t0));
    put32(out + 8, samples);
    *out_len = o;
    return rc;
}

static Result handle_send(const u8* in, u32 in_len, u8* out, u16* out_len) {
    if(in_len < 3) return -3;
    IrSignal s;
    memset(&s, 0, sizeof(s));
    snprintf(s.name, sizeof(s.name), "net");
    u8 kind = in[0], style = in[1], repeats = in[2];
    in += 3;
    in_len -= 3;
    if(kind == 0) {
        if(in_len < 24) return -3;
        memcpy(s.protocol, in, 15);
        s.address = get32(in + 16);
        s.command = get32(in + 20);
    } else {
        if(in_len < 6) return -3;
        s.raw = true;
        s.frequency = get32(in);
        u32 count = in[4] | (in[5] << 8);
        static u32 timings[0x2000]; // copied out: the timings aren't 4-byte aligned in the request
        if(in_len < 6 + count * 4 || count > sizeof(timings) / 4) return -3;
        memcpy(timings, in + 6, count * 4);
        s.data = timings;
        s.data_n = count;
    }
    IrTimings tm;
    ir_timings_init(&tm);
    Result rc = -7; // unsupported protocol
    u32 bytes = 0;
    if(irfile_signal_timings(&s, repeats ? repeats : 1, &tm)) rc = app_send(&tm, style ? IrCarrierClean : IrCarrierDense, &bytes);
    ir_timings_free(&tm);
    put32(out, bytes);
    *out_len = 4;
    return rc;
}

// Handles one hardware request (hw_lock held); out/out_len is the reply payload.
static Result handle_hw(u8 op, const u8* in, u32 in_len, u8* out, u16* out_len) {
    switch(op) {
    case OP_TAKEOVER:
        return app_hw_direct(in_len ? in[0] : IRHW_STEAL);
    case OP_RELEASE:
        app_hw_iru();
        return 0;
    case OP_SEND:
        return handle_send(in, in_len, out, out_len);
    case OP_IRU_SEND: {
        if(in_len < 1) return -3;
        u32 us = 0;
        Result rc = app_iru_send(in[0], in + 1, in_len - 1, &us);
        put32(out, us);
        *out_len = 4;
        return rc;
    }
    }

    if(!irhw_owned()) return -2; // TAKEOVER first
    switch(op) {
    case OP_REG_READ:
        if(in_len < 2) return -3;
        *out_len = in[1];
        return sc_read(in[0], out, in[1]);
    case OP_REG_WRITE:
        if(in_len < 2) return -3;
        return sc_write(in[0], in + 1, in_len - 1);
    case OP_GPIO: {
        if(in_len < 12) return -3;
        u32 data = 0;
        Result rc = irhw_gpio(get32(in), get32(in + 4), get32(in + 8), &data);
        put32(out, data);
        *out_len = 4;
        return rc;
    }
    case OP_I2C: {
        if(in_len < 3) return -3;
        u32 rlen = in[1] | (in[2] << 8);
        if(rlen > 0x100) return -3;
        if(in_len > 3) {
            Result rc = irhw_i2c_write(in[0], in + 3, in_len - 3);
            if(R_FAILED(rc)) return rc;
        }
        if(!rlen) return 0;
        *out_len = rlen;
        return irhw_i2c_read(in[0], out, rlen);
    }
    case OP_IPC: {
        if(in_len < 7 || (in_len - 3) % 4) return -3;
        u32 osize = in[1] | (in[2] << 8), words[16];
        u32 n = (in_len - 3) / 4;
        if(n > 16 || osize > OUT_MAX - 64) return -3;
        memcpy(words, in + 3, n * 4);
        u32 reply[16];
        Result rc = irhw_ipc(in[0], words, n, out + 64, osize, reply);
        memcpy(out, reply, 64);
        *out_len = 64 + osize;
        return rc;
    }
    case OP_BENCH: {
        if(in_len < 2) return -3;
        u32 mn, avg, mx;
        Result rc = irhw_bench(in[0] | (in[1] << 8), &mn, &avg, &mx);
        put32(out, mn);
        put32(out + 4, avg);
        put32(out + 8, mx);
        *out_len = 12;
        return rc;
    }
    case OP_TX:
        return direct_tx(in, in_len, out, out_len);
    case OP_RX:
        if(in_len < 2) return -3;
        return direct_rx(in[0] | (in[1] << 8), out, out_len);
    }
    return -4; // unknown op
}

#define IN_MAX 0x8000
#define MAX_CLIENTS 3

// One thread per client, so a hardware call that never returns (it holds hw_lock) doesn't take
// log, get, shot and update down with it.
static struct Client {
    Thread thread;
    int fd;
    volatile bool busy;
    u8* in;
    u8* out;
} clients[MAX_CLIENTS];

static void serve_client(int fd, u8* in, u8* out) {
    for(;;) {
        u8 hdr[3];
        if(!recv_all(fd, hdr, 3)) return;
        u32 len = hdr[1] | (hdr[2] << 8);
        if(len > IN_MAX || !recv_all(fd, in, len)) return;
        u16 out_len = 0;
        Result res;
        u8 op = hdr[0];
        if(op == OP_SHOT || op == OP_KEYS || op == OP_TOUCH || op == OP_LOG || op == OP_GET_FILE) {
            res = handle_debug(op, in, len, out, &out_len);
        } else if(op == OP_SET_TRIM) {
            put32(out, (u32)ir_mark_trim_us);
            out_len = 4;
            res = len >= 4 ? 0 : -3;
            if(!res) {
                ir_mark_trim_us = (int32_t)get32(in);
                trim_overridden = true;
            }
        } else if(op == OP_PING) {
            memcpy(out, "3DSir", 5);
            out_len = 5;
            res = 0;
        } else if(op == OP_UPDATE || op == OP_UPDATE_DONE) {
            res = handle_update(op, in, len);
        } else if(op == OP_PUT_FILE) {
            res = handle_put_file(in, len);
        } else {
            LightLock_Lock(&hw_lock);
            res = handle_hw(op, in, len, out, &out_len);
            LightLock_Unlock(&hw_lock);
        }
        if(!send_reply(fd, op, res, out, out_len)) return;
    }
}

static void client_main(void* arg) {
    struct Client* c = arg;
    serve_client(c->fd, c->in, c->out);
    close(c->fd);
    logp("net: client gone\n");
    c->busy = false;
}

static void net_main(void* arg) {
    (void)arg;
    while(!net_quit) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        if(!wait_readable(listen_fd)) continue;
        int fd = accept(listen_fd, (struct sockaddr*)&client, &clen);
        if(fd < 0) {
            svcSleepThread(MS(100));
            continue;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
        struct Client* c = NULL;
        for(int i = 0; i < MAX_CLIENTS && !c; i++)
            if(!clients[i].busy) c = &clients[i];
        if(!c) {
            logp("net: client %s refused, %d busy\n", inet_ntoa(client.sin_addr), MAX_CLIENTS);
            close(fd);
            continue;
        }
        if(c->thread) {
            threadJoin(c->thread, U64_MAX); // it already finished: busy is cleared last
            threadFree(c->thread);
            c->thread = NULL;
        }
        if(!c->in) c->in = memalign(4, IN_MAX);
        if(!c->out) c->out = memalign(4, OUT_MAX);
        if(!c->in || !c->out) {
            close(fd);
            continue;
        }
        logp("net: client %s\n", inet_ntoa(client.sin_addr));
        c->fd = fd;
        c->busy = true;
        c->thread = threadCreate(client_main, c, 0x8000, 0x30, -2, false);
        if(!c->thread) {
            c->busy = false;
            close(fd);
        }
    }
}

void net_start(void) {
    soc_buf = memalign(0x1000, SOC_BUFFERSIZE);
    if(!soc_buf || R_FAILED(socInit(soc_buf, SOC_BUFFERSIZE))) {
        logp("net: socInit failed (no wifi?)\n");
        free(soc_buf);
        soc_buf = NULL;
        return;
    }
    listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(NET_PORT), .sin_addr.s_addr = INADDR_ANY};
    if(listen_fd < 0 || bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) || listen(listen_fd, 1)) {
        logp("net: can't listen on %d\n", NET_PORT);
        return;
    }
    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL, 0) | O_NONBLOCK);
    struct in_addr ip = {.s_addr = gethostid()};
    snprintf(net_addr, sizeof(net_addr), "%s:%d", inet_ntoa(ip), NET_PORT);
    logp("net: listening on %s\n", net_addr);
    net_thread = threadCreate(net_main, NULL, 0x8000, 0x30, -2, false);
}

void net_stop(void) {
    if(!soc_buf) return;
    net_quit = true;
    if(net_thread) {
        threadJoin(net_thread, U64_MAX);
        threadFree(net_thread);
    }
    // Idle clients notice net_quit within 100 ms. One stuck in a hardware call is left behind
    // (the process exit takes it); its buffers are leaked with it.
    for(int i = 0; i < MAX_CLIENTS; i++) {
        struct Client* c = &clients[i];
        if(!c->thread) continue;
        if(R_SUCCEEDED(threadJoin(c->thread, 1000000000ULL))) {
            threadFree(c->thread);
            free(c->in);
            free(c->out);
        } else {
            logp("net: client thread %d stuck, leaving it\n", i);
        }
        c->thread = NULL;
        c->in = c->out = NULL;
    }
    if(listen_fd >= 0) close(listen_fd);
    socExit();
    free(soc_buf);
    soc_buf = NULL;
}
