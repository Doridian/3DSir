// The app's screens: the root menu, the remote browser, a remote's buttons (send, learn, rename, move,
// delete), learning, settings and the hardware tools.

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app.h"
#include "irdb.h"
#include "irdecode.h"
#include "irhw.h"
#include "log.h"

#define LIST_Y 26
#define LIST_ROWS 12
#define MS(x) ((s64)(x) * 1000000LL)

static void open_remote(const char* path);

// ---------------------------------------------------------------- names

// Characters FAT and the Flipper don't take in a file name.
static bool bad_file_name(const char* s) {
    if(!*s || *s == '.' || *s == ' ') return true;
    return strpbrk(s, "/\\:*?\"<>|") != NULL;
}

static const char* base_name(const char* path) {
    const char* b = strrchr(path, '/');
    return b ? b + 1 : path;
}

// "sdmc:/3ds/3dsir/TVs/LG.ir" -> "LG"
static void remote_name(const char* path, char* out, size_t max) {
    snprintf(out, max, "%s", base_name(path));
    size_t n = strlen(out);
    if(n > 3 && !strcasecmp(out + n - 3, ".ir")) out[n - 3] = 0;
}

static bool file_exists(const char* path) {
    struct stat st;
    return !stat(path, &st);
}

// ---------------------------------------------------------------- sending (jobs)

static IrFile remote; // the remote on screen
static char remote_path[256]; // empty: the built-in codes (not saved)
static bool remote_ro; // in the database's pack: X saves a copy instead of editing

static void save_copy(const char* from); // into Remotes, under a name the user gives
static UiList remote_list;

static IrSignal send_sig;
static Result send_rc;
static IrTimings send_tm;

static void send_run(void) {
    send_rc = 0;
    if(!irfile_signal_timings(&send_sig, repeats, &send_tm)) {
        send_rc = -7;
        return;
    }
    // Keep resending the same waveform (same RC5/RC6 toggle bit) while A is held.
    do {
        send_rc = app_send(&send_tm, style, NULL);
        if(R_FAILED(send_rc)) break;
        svcSleepThread(MS(40));
    } while(ui_keys_held & KEY_A);
}

static void send_done(void) {
    if(send_rc == -7) toast("Can't send %s codes", send_sig.raw ? "raw" : send_sig.protocol);
    else if(send_rc == -2) toast("The IR hardware is busy (taken over)");
    else if(R_FAILED(send_rc)) toast("Send failed: %08lX", send_rc);
    else {
        char d[64];
        irfile_describe(&send_sig, d, sizeof(d));
        logp("sent %s (%s)\n", send_sig.name, d);
    }
}

static void send_signal(const IrSignal* s) {
    send_sig = *s; // the raw data stays the file's: nothing edits it while the job runs
    job_start(s->name, send_run, send_done, JOB_QUIET);
    job_note("Sending %s", s->name);
}

// Every signal named like the selected one (all "Power" codes of a universal library), TV-B-Gone style.
static char sweep_name[64];
static size_t sweep_count;

static void sweep_run(void) {
    size_t total = 0, k = 0;
    for(size_t i = 0; i < remote.n; i++) total += !strcmp(remote.sig[i].name, sweep_name);
    sweep_count = 0;
    for(size_t i = 0; i < remote.n && !job_cancel; i++) {
        IrSignal* s = &remote.sig[i];
        if(strcmp(s->name, sweep_name)) continue;
        char d[64];
        irfile_describe(s, d, sizeof(d));
        job_note("%zu / %zu: %s", ++k, total, d);
        if(!irfile_signal_timings(s, repeats, &send_tm)) {
            logp("skipped %s: unsupported\n", d);
            continue;
        }
        Result rc = app_send(&send_tm, style, NULL);
        if(R_FAILED(rc)) {
            logp("%s: %08lX\n", d, rc);
            break;
        }
        sweep_count++;
        svcSleepThread(MS(150)); // let the TV settle between codes
    }
}

static void sweep_done(void) {
    toast("Sent %zu \"%s\" codes", sweep_count, sweep_name);
}

// ---------------------------------------------------------------- learning

enum { LEARN_NEW_REMOTE, LEARN_ADD, LEARN_REPLACE };
static struct {
    int target; // LEARN_*
    size_t index; // LEARN_REPLACE: the button
    char dir[256]; // LEARN_NEW_REMOTE: where the remote goes
    u32 t[512];
    size_t n;
    Result rc;
    IrDecoded d;
    IrSignal sig; // what was learned
} learn;

static void learn_start(void);

static void learn_run(void) {
    learn.n = 0;
    learn.rc = app_hw_direct(IRHW_STEAL);
    if(R_SUCCEEDED(learn.rc)) learn.n = app_learn_capture(learn.t, sizeof(learn.t) / sizeof(learn.t[0]), &job_cancel);
    else logp("takeover failed: %08lX\n", learn.rc);
    app_hw_iru();
}

static const Screen learned_screen;

static void learn_done(void) {
    if(job_cancel) return;
    if(R_FAILED(learn.rc)) {
        msg_box("Learn", "Couldn't take the IR receiver over (%08lX). Learning needs Luma3DS.", learn.rc);
        return;
    }
    logp("learn: %zu bursts\n", learn.n);
    if(!ir_decode(learn.t, learn.n, &learn.d)) {
        confirm("Not learned", learn_start, "%s\n\nTry again?", ir_decode_hint(learn.t, learn.n));
        return;
    }
    memset(&learn.sig, 0, sizeof(learn.sig));
    snprintf(learn.sig.protocol, sizeof(learn.sig.protocol), "%s", learn.d.protocol);
    learn.sig.address = learn.d.address;
    learn.sig.command = learn.d.command;
    logp("learned %s %02lX / %02lX\n", learn.d.protocol, learn.d.address, learn.d.command);
    app_screen(&learned_screen);
}

static void learn_start(void) {
    job_start("Learn", learn_run, learn_done, JOB_CANCEL);
    job_note("Point the remote at the IR port on the back of the 3DS, a few cm away, and press the button once.");
}

static void learned_save_named(const char* name);
static void learned_remote_named(const char* name);

static void learned_input(u32 down, u32 repeat) {
    (void)repeat;
    if(down & KEY_A) {
        app_back();
        if(learn.target == LEARN_REPLACE) {
            learned_save_named(remote.sig[learn.index].name);
        } else {
            entry_text("Name the button", "The name the button gets in the remote (Flipper's universal remotes use Power, "
                       "Vol_up, Vol_dn, Ch_next, Ch_prev, Mute).", "", 63, learned_save_named);
        }
    } else if(down & KEY_X) {
        send_signal(&learn.sig);
    } else if(down & KEY_Y) {
        app_back();
        learn_start();
    } else if(down & KEY_B) {
        app_back();
    }
}

static void learned_draw(void) {
    ui_header("Learned", NULL);
    ui_text(12, 30, S_SMALL, ui_col[COL_DIM], "Protocol");
    ui_text(12, 44, 0.8f, ui_col[COL_ACCENT], "%s", learn.d.protocol);
    ui_text(200, 30, S_SMALL, ui_col[COL_DIM], "Address");
    ui_text(200, 44, 0.8f, ui_col[COL_TEXT], "%02lX", learn.d.address);
    ui_text(300, 30, S_SMALL, ui_col[COL_DIM], "Command");
    ui_text(300, 44, 0.8f, ui_col[COL_TEXT], "%02lX", learn.d.command);
    char s[256];
    snprintf(s, sizeof(s), "From %zu bursts of light.%s", learn.n,
             learn.d.last_bit_guessed ? " The protocol's last bit can't be seen by the 3DS's receiver and was "
                                        "taken as 0, the usual value. If the code doesn't work, the address's top bit "
                                        "may be 1."
                                      : "");
    ui_text_wrap(12, 80, UI_W - 24, S_SMALL, ui_col[COL_TEXT], s);
    const char* what = learn.target == LEARN_REPLACE ? "replaces the button's code" :
                       learn.target == LEARN_ADD    ? "adds it to the remote" :
                                                      "starts a new remote with it";
    snprintf(s, sizeof(s), BTN_A " %s. " BTN_X " sends it to check, " BTN_Y " learns again.", what);
    ui_text_wrap(12, 130, UI_W - 24, S_SMALL, ui_col[COL_DIM], s);
    ui_footer(BTN_A " save   " BTN_X " test   " BTN_Y " again   " BTN_B " discard");
}

static const Screen learned_screen = {NULL, learned_input, learned_draw, NULL};

static bool remote_save(void) {
    if(!*remote_path || remote_ro) return true;
    if(irfile_save(&remote, remote_path)) {
        irdb_touched(remote_path);
        return true;
    }
    msg_box("Couldn't save", "Writing %s failed. The SD card may be full or write-protected.", remote_path);
    return false;
}

static void learned_save_named(const char* name) {
    if(!*name) {
        toast("A button needs a name");
        entry_text("Name the button", "The name the button gets in the remote.", "", 63, learned_save_named);
        return;
    }
    snprintf(learn.sig.name, sizeof(learn.sig.name), "%s", name);
    if(learn.target == LEARN_NEW_REMOTE) {
        entry_text("Name the remote", "The remote is saved as <name>.ir with this button in it.", "", 60,
                   learned_remote_named);
        return;
    }
    if(learn.target == LEARN_REPLACE) {
        irfile_replace_code(&remote, learn.index, &learn.sig);
        remote_list.sel = learn.index;
    } else {
        irfile_add(&remote, &learn.sig);
        remote_list.sel = remote.n - 1;
    }
    if(remote_save()) toast("Saved %s", learn.sig.name);
}

static void learned_remote_named(const char* name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ir", learn.dir, name);
    if(bad_file_name(name) || file_exists(path)) {
        toast(bad_file_name(name) ? "Not a usable file name" : "%s.ir exists already", name);
        entry_text("Name the remote", "The remote is saved as <name>.ir with this button in it.", name, 60,
                   learned_remote_named);
        return;
    }
    IrFile f;
    irfile_init(&f);
    irfile_add(&f, &learn.sig);
    bool ok = irfile_save(&f, path);
    irfile_free(&f);
    if(!ok) {
        msg_box("Couldn't save", "Writing %s failed.", path);
        return;
    }
    irdb_touched(path);
    open_remote(path);
}

static void learn_new_remote(const char* dir) {
    learn.target = LEARN_NEW_REMOTE;
    snprintf(learn.dir, sizeof(learn.dir), "%s", dir);
    learn_start();
}

// ---------------------------------------------------------------- a remote: its buttons

static bool moving;

static int same_name_count(size_t i) {
    int k = 0;
    for(size_t j = 0; j < remote.n; j++) k += !strcmp(remote.sig[j].name, remote.sig[i].name);
    return k;
}

static void act_learn_add(void) {
    learn.target = LEARN_ADD;
    learn_start();
}

static void act_relearn(void) {
    learn.target = LEARN_REPLACE;
    learn.index = remote_list.sel;
    learn_start();
}

static void renamed(const char* name) {
    if(!*name) {
        toast("A button needs a name");
        return;
    }
    snprintf(remote.sig[remote_list.sel].name, sizeof(remote.sig[0].name), "%s", name);
    if(remote_save()) toast("Renamed");
}

static void act_rename(void) {
    entry_text("Rename button", "The button's new name.", remote.sig[remote_list.sel].name, 63, renamed);
}

static void act_move(void) {
    moving = true;
}

static void delete_yes(void) {
    irfile_remove(&remote, remote_list.sel);
    ui_list_fix(&remote_list, remote.n, LIST_ROWS);
    if(remote_save()) toast("Deleted");
}

static void act_delete(void) {
    confirm("Delete button", delete_yes, "Delete \"%s\" from this remote?", remote.sig[remote_list.sel].name);
}

static void remote_input(u32 down, u32 repeat) {
    int n = remote.n;
    if(moving) {
        int s = remote_list.sel;
        if((repeat & KEY_UP) && s > 0) irfile_move(&remote, s, s - 1), remote_list.sel--;
        if((repeat & KEY_DOWN) && s < n - 1) irfile_move(&remote, s, s + 1), remote_list.sel++;
        ui_list_fix(&remote_list, n, LIST_ROWS);
        if(down & (KEY_A | KEY_B)) {
            moving = false;
            if(remote_save()) toast("Order saved");
        }
        return;
    }
    ui_list_nav(&remote_list, n, LIST_ROWS, repeat);
    if((down & KEY_L) && repeats > 1) repeats--, settings_save(), toast("Each press sends the code %u time%s", repeats, repeats == 1 ? "" : "s");
    if((down & KEY_R) && repeats < 10) repeats++, settings_save(), toast("Each press sends the code %u times", repeats);
    if(down & KEY_B) {
        app_back();
        return;
    }
    if((down & KEY_A) && n) send_signal(&remote.sig[remote_list.sel]);
    if((down & KEY_Y) && n && same_name_count(remote_list.sel) > 1) {
        snprintf(sweep_name, sizeof(sweep_name), "%s", remote.sig[remote_list.sel].name);
        job_start("Sending every code", sweep_run, sweep_done, JOB_CANCEL);
    }
    if(down & KEY_X) {
        if(!*remote_path) {
            toast("The built-in codes can't be edited");
        } else if(remote_ro) {
            save_copy(remote_path);
        } else if(!n) {
            act_learn_add();
        } else {
            static const MenuItem items[] = {
                {"Learn a new button", act_learn_add}, {"Re-learn this button", act_relearn},
                {"Rename", act_rename},                {"Move", act_move},
                {"Delete", act_delete},
            };
            popup(remote.sig[remote_list.sel].name, items, 5);
        }
    }
}

static void remote_draw(void) {
    char title[96], right[48];
    remote_name(*remote_path ? remote_path : "Built-in test codes", title, sizeof(title));
    snprintf(right, sizeof(right), "%zu button%s  x%u  %s", remote.n, remote.n == 1 ? "" : "s", repeats,
             style == IrCarrierClean ? "clean" : "dense");
    ui_header(title, right);
    const float lw = 232;
    if(!remote.n) {
        ui_text_wrap(12, 40, UI_W - 24, S_BODY, ui_col[COL_DIM],
                     "No buttons yet.\n\n" BTN_X " learns one: point the remote at the 3DS's IR port and press it.");
    }
    for(int i = remote_list.top; i < (int)remote.n && i < remote_list.top + LIST_ROWS; i++) {
        float y = LIST_Y + (i - remote_list.top) * LINE_H;
        bool sel = i == remote_list.sel;
        if(sel) ui_sel(4, y - 1, lw, LINE_H);
        if(sel && moving) ui_rect(4, y - 1, lw, LINE_H, ui_col[COL_CURSOR]);
        char s[80];
        snprintf(s, sizeof(s), "%s", remote.sig[i].name);
        ui_text(12, y, S_BODY, ui_col[sel && moving ? COL_BG : sel ? COL_TEXT : COL_DIM], "%s", ui_fit(s, lw - 14, S_BODY));
    }
    if(remote.n > LIST_ROWS) // scroll bar
        ui_rect(lw + 6, LIST_Y + (float)remote_list.top / remote.n * LIST_ROWS * LINE_H, 2,
                (float)LIST_ROWS / remote.n * LIST_ROWS * LINE_H, ui_col[COL_SEL]);
    if(remote.n) {
        const IrSignal* s = &remote.sig[remote_list.sel];
        float x = lw + 14, w = UI_W - x - 6, y = LIST_Y;
        ui_rect(x - 4, y - 2, w + 8, 120, ui_col[COL_PANEL]);
        if(s->raw) {
            ui_text(x, y, S_SMALL, ui_col[COL_DIM], "Raw signal");
            ui_text(x, y + 14, S_BODY, ui_col[COL_ACCENT], "%lu kHz", (unsigned long)(s->frequency + 500) / 1000);
            ui_text(x, y + 32, S_SMALL, ui_col[COL_TEXT], "%zu samples", s->data_n);
        } else {
            ui_text(x, y, S_SMALL, ui_col[COL_DIM], "Protocol");
            ui_text(x, y + 13, S_BODY, ui_col[COL_ACCENT], "%s", s->protocol);
            ui_text(x, y + 33, S_SMALL, ui_col[COL_DIM], "Address");
            ui_text(x + 60, y + 33, S_SMALL, ui_col[COL_TEXT], "%02lX", (unsigned long)s->address);
            ui_text(x, y + 47, S_SMALL, ui_col[COL_DIM], "Command");
            ui_text(x + 60, y + 47, S_SMALL, ui_col[COL_TEXT], "%02lX", (unsigned long)s->command);
        }
        int same = same_name_count(remote_list.sel);
        if(same > 1) {
            char t[128];
            snprintf(t, sizeof(t), BTN_Y " sends all %d \"%s\" codes in turn.", same, s->name);
            ui_text_wrap(x, y + 70, w, S_SMALL, ui_col[COL_TEXT], t);
        }
        ui_text_wrap(x, y + 124, w, S_SMALL, ui_col[COL_DIM], "Hold " BTN_A " to keep sending. " BTN_L "/" BTN_R " repeats per press.");
    }
    if(moving) ui_footer(BTN_UD " move   " BTN_A " done");
    else if(!*remote_path) ui_footer(BTN_A " send   " BTN_Y " send all with this name   " BTN_B " back");
    else if(remote_ro) ui_footer(BTN_A " send   " BTN_X " save a copy   " BTN_Y " send all   " BTN_B " back");
    else ui_footer(BTN_A " send   " BTN_X " edit   " BTN_Y " send all   " BTN_B " back");
}

// Big buttons for the remote's codes, a page at a time.
static void remote_bottom(void) {
    if(moving || !remote.n) {
        if(moving) {
            if(ui_button_rep(4, 4, 154, 60, ui_col[COL_BAR], " Up")) app_press(KEY_UP);
            if(ui_button_rep(162, 4, 154, 60, ui_col[COL_BAR], " Down")) app_press(KEY_DOWN);
            if(ui_button(4, 70, 312, 40, ui_col[COL_SEL], "Done")) app_press(KEY_A);
        } else if(*remote_path && !remote_ro && ui_button(4, 4, 312, 60, ui_col[COL_SEL], "Learn a button")) {
            app_press(KEY_X);
        }
        if(ui_button(4, UI_H - 34, 312, 30, ui_col[COL_PANEL], "Back")) app_press(KEY_B);
        return;
    }
    const int cols = 2, rows = 5, per = cols * rows;
    int page = remote_list.sel / per, pages = (remote.n + per - 1) / per;
    float w = (UI_BW - 12) / cols, h = 36;
    for(int k = 0; k < per; k++) {
        int i = page * per + k;
        if(i >= (int)remote.n) break;
        float x = 4 + (k % cols) * (w + 4), y = 4 + (k / cols) * (h + 4);
        if(ui_button(x, y, w, h, i == remote_list.sel ? ui_col[COL_SEL] : ui_col[COL_BAR], remote.sig[i].name)) {
            remote_list.sel = i;
            ui_list_fix(&remote_list, remote.n, LIST_ROWS);
            app_press(KEY_A);
        }
    }
    float y = UI_H - 34;
    if(pages > 1) {
        if(ui_button(4, y, 60, 30, ui_col[COL_PANEL], "")) remote_list.sel = (page + pages - 1) % pages * per;
        if(ui_button(68, y, 60, 30, ui_col[COL_PANEL], "")) remote_list.sel = (page + 1) % pages * per;
        ui_list_fix(&remote_list, remote.n, LIST_ROWS);
    }
    if(*remote_path && ui_button(132, y, 90, 30, ui_col[COL_PANEL], remote_ro ? "Save copy" : "Edit")) app_press(KEY_X);
    if(ui_button(226, y, 90, 30, ui_col[COL_PANEL], "Back")) app_press(KEY_B);
}

static const Screen remote_screen = {NULL, remote_input, remote_draw, remote_bottom};

static void open_remote(const char* path) {
    IrFile f;
    if(!irdb_load(path, &f)) {
        msg_box("Can't open", "%s", path);
        return;
    }
    irfile_free(&remote);
    remote = f;
    snprintf(remote_path, sizeof(remote_path), "%s", path);
    remote_ro = irdb_is_packed(path);
    remote_list = (UiList){0};
    moving = false;
    logp("%s: %zu signals\n", base_name(path), remote.n);
    app_screen(&remote_screen);
}

// ---------------------------------------------------------------- built-in codes

static const struct {
    const char *name, *protocol;
    u32 address, command;
} builtin[] = {
    {"Samsung Vol+", "Samsung32", 0x07, 0x07}, {"Samsung Power", "Samsung32", 0x07, 0x02},
    {"LG Vol+", "NEC", 0x04, 0x02},            {"LG Power", "NEC", 0x04, 0x08},
    {"Sony Vol+", "SIRC", 0x01, 0x12},         {"Sony Power", "SIRC", 0x01, 0x15},
    {"Philips Vol+", "RC5", 0x00, 0x10},       {"Philips Power", "RC5", 0x00, 0x0C},
};

static void open_builtin(void) {
    irfile_free(&remote);
    irfile_init(&remote);
    for(size_t i = 0; i < sizeof(builtin) / sizeof(builtin[0]); i++) {
        IrSignal s = {0};
        snprintf(s.name, sizeof(s.name), "%s", builtin[i].name);
        snprintf(s.protocol, sizeof(s.protocol), "%s", builtin[i].protocol);
        s.address = builtin[i].address;
        s.command = builtin[i].command;
        irfile_add(&remote, &s);
    }
    *remote_path = 0;
    remote_ro = false;
    remote_list = (UiList){0};
    moving = false;
    app_screen(&remote_screen);
}

// ---------------------------------------------------------------- the remote database (irdb.c, irdbget.c)

static struct {
    char sha[48], err[160];
    bool ok;
} dbj;

static void db_progress(size_t done, size_t todo) {
    if(done % 8 == 0) job_note("Indexing remotes: %zu of %zu", done, todo);
}

static void db_refresh_run(void) {
    job_note("Loading the remote index...");
    dbj.ok = irdb_refresh(&job_cancel, db_progress);
}

static void db_rebuild_run(void) {
    job_note("Indexing remotes...");
    dbj.ok = irdb_rebuild(&job_cancel, db_progress);
}

static void db_check_run(void) {
    *dbj.err = 0;
    dbj.ok = irdb_latest(dbj.sha, sizeof(dbj.sha), dbj.err, sizeof(dbj.err));
}

static void db_install_run(void) {
    *dbj.err = 0;
    irdb_open();
    dbj.ok = irdb_install(dbj.sha, &job_cancel, dbj.err, sizeof(dbj.err));
    if(dbj.ok) dbj.ok = irdb_refresh(&job_cancel, db_progress); // the user's own remotes too
}

static void open_finder(void);

static void db_install_done(void) {
    if(!dbj.ok) {
        msg_box("Flipper-IRDB", "%s", *dbj.err ? dbj.err : "Stopped. The remote index will be finished next time.");
        return;
    }
    confirm("Flipper-IRDB", open_finder,
            "Installed: %zu remotes the 3DS can recognize. Remotes > Flipper-IRDB lists them all.\n\nFind your "
            "remote among them now?",
            irdb_detectable());
}

static void db_install(void) {
    job_start("Flipper-IRDB", db_install_run, db_install_done, JOB_CANCEL | JOB_CPU);
}

static void db_check_done(void) {
    if(!dbj.ok) {
        msg_box("Flipper-IRDB", "%s", dbj.err);
        return;
    }
    char have[48];
    irdb_pack_version(have, sizeof(have));
    if(!strcmp(have, dbj.sha)) {
        confirm("Flipper-IRDB", db_install, "The newest version (%.7s) is installed already.\n\nDownload it again?",
                dbj.sha);
        return;
    }
    confirm("Flipper-IRDB", db_install,
            "%s version %.7s from GitHub: about 18 MB to download, ~9000 remotes (65 MB on the SD card). This "
            "takes a few minutes.",
            *have ? "Update to" : "Download", dbj.sha);
}

static void db_check(void) {
    job_start("Flipper-IRDB", db_check_run, db_check_done, JOB_CANCEL | JOB_CPU);
}

static void db_rebuild_done(void) {
    if(dbj.ok) toast("%zu remotes indexed", irdb_files());
}

static void db_rebuild(void) {
    job_start("Remote index", db_rebuild_run, db_rebuild_done, JOB_CANCEL | JOB_CPU);
}

static void db_input(u32 down, u32 repeat) {
    (void)repeat;
    if(down & KEY_A) db_check();
    if(down & KEY_Y) db_rebuild();
    if(down & KEY_B) app_back();
}

static void db_draw(void) {
    ui_header("Remote database", NULL);
    char v[48], s[512];
    irdb_pack_version(v, sizeof(v));
    if(*v) snprintf(s, sizeof(s), "Flipper-IRDB version %.7s is installed.", v);
    else snprintf(s, sizeof(s), "The Flipper-IRDB isn't installed.");
    float y = ui_text_wrap(12, 30, UI_W - 24, S_BODY, ui_col[COL_TEXT], s);
    if(irdb_loaded() && !job_busy()) { // a job may be changing the index
        snprintf(s, sizeof(s), "%zu remote files indexed, %zu with codes the 3DS can recognize.", irdb_files(),
                 irdb_detectable());
        y = ui_text_wrap(12, y + 6, UI_W - 24, S_BODY, ui_col[COL_TEXT], s);
    }
    ui_text_wrap(12, y + 14, UI_W - 24, S_SMALL, ui_col[COL_DIM],
                 "The Flipper-IRDB (github.com/Lucaslhm/Flipper-IRDB) collects thousands of remotes' codes. "
                 "\"Find my remote\" looks for yours in it, and in every .ir file in " ROOT_DIR ". Remotes > "
                 "Flipper-IRDB lists all of its remotes.\n\n"
                 BTN_A " downloads it over Wi-Fi, or updates it to the newest version.\n" BTN_Y
                 " indexes every remote again (after copying or changing .ir files on a PC).");
    ui_footer(BTN_A " download / update   " BTN_Y " re-index   " BTN_B " back");
}

static const Screen db_screen = {NULL, db_input, db_draw, NULL};

static void open_db(void) {
    app_screen(&db_screen);
}

// ---------------------------------------------------------------- find my remote

#define HIST 6

static struct {
    int ask; // the button asked for (IRDB_BTN_OTHER: any)
    char hist[HIST][96]; // what each press found
    int nhist;
    u32 t[512];
    size_t n;
    Result rc;
    UiList l; // the matches
} fnd;

static const Screen finder_screen, matches_screen;

static void hist_add(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void hist_add(const char* fmt, ...) {
    if(fnd.nhist == HIST) memmove(fnd.hist[0], fnd.hist[1], sizeof(fnd.hist[0]) * (HIST - 1)), fnd.nhist--;
    va_list va;
    va_start(va, fmt);
    vsnprintf(fnd.hist[fnd.nhist++], sizeof(fnd.hist[0]), fmt, va);
    va_end(va);
}

static size_t finder_left(void) {
    const IrdbFile* const* c;
    return irdb_find_candidates(&c);
}

static void show_matches(void) {
    fnd.l = (UiList){0};
    if(app_top() != &matches_screen) app_screen(&matches_screen);
}

// After a press or a skip: the next question, or the answer.
static void finder_next(void) {
    if(!irdb_find_presses()) return; // still the first press: Power, or any
    int b = irdb_find_next();
    if(b < 0 || finder_left() < 2) {
        fnd.ask = IRDB_BTN_OTHER; // back from the matches, any press narrows them further
        show_matches();
        return;
    }
    fnd.ask = b;
}

static void listen_run(void) {
    fnd.n = 0;
    fnd.rc = app_hw_direct(IRHW_STEAL);
    if(R_SUCCEEDED(fnd.rc)) fnd.n = app_learn_capture(fnd.t, sizeof(fnd.t) / sizeof(fnd.t[0]), &job_cancel);
    app_hw_iru();
}

static void listen_start(void);

static void learn_instead(void) {
    learn.target = LEARN_NEW_REMOTE;
    snprintf(learn.dir, sizeof(learn.dir), "%s", ROOT_DIR);
    app_screen(&learned_screen);
}

static void listen_done(void) {
    if(job_cancel) return;
    if(R_FAILED(fnd.rc)) {
        msg_box("Find my remote", "Couldn't take the IR receiver over (%08lX). This needs Luma3DS.", fnd.rc);
        return;
    }
    IrDecoded d;
    IrKey k;
    if(!ir_decode(fnd.t, fnd.n, &d) || !irdb_decoded_key(&d, &k)) {
        confirm("Not recognized", listen_start, "%s\n\nTry again?", ir_decode_hint(fnd.t, fnd.n));
        return;
    }
    const char* what = irdb_button_label(fnd.ask);
    logp("find: %s -> %s %02lX / %02lX\n", what, d.protocol, d.address, d.command);
    size_t before = irdb_find_presses() ? finder_left() : irdb_detectable();
    if(!irdb_find_press(&k, fnd.ask)) {
        if(!irdb_find_presses()) {
            // Nothing sends it: offer to learn the remote button by button, starting with this one.
            learn.d = d;
            learn.n = fnd.n;
            memset(&learn.sig, 0, sizeof(learn.sig));
            snprintf(learn.sig.protocol, sizeof(learn.sig.protocol), "%s", d.protocol);
            learn.sig.address = d.address;
            learn.sig.command = d.command;
            confirm("Not in the database", learn_instead,
                    "No remote in the database sends %s %02lX / %02lX.\n\nLearn your remote button by button instead, "
                    "starting with this one?",
                    d.protocol, d.address, d.command);
            return;
        }
        hist_add("%s: %s %02lX/%02lX, none of the %zu sends it", what, d.protocol, d.address, d.command, before);
        toast("None of the matching remotes sends that code");
        finder_next();
        return;
    }
    hist_add("%s: %s %02lX/%02lX, %zu remote%s left", what, d.protocol, d.address, d.command, finder_left(),
             finder_left() == 1 ? "" : "s");
    finder_next();
}

static void listen_start(void) {
    job_start("Listening", listen_run, listen_done, JOB_CANCEL);
    job_note("Point the remote at the IR port on the back of the 3DS, a few cm away, and press %s once.",
             fnd.ask == IRDB_BTN_OTHER ? "any button" : irdb_button_label(fnd.ask));
}

static void finder_input(u32 down, u32 repeat) {
    (void)repeat;
    if(down & KEY_A) listen_start();
    if(down & KEY_X) { // the remote has no such button
        if(fnd.ask == IRDB_BTN_OTHER) return;
        irdb_find_skip(fnd.ask);
        if(irdb_find_presses())
            hist_add("%s: skipped, %zu remote%s left", irdb_button_label(fnd.ask), finder_left(),
                     finder_left() == 1 ? "" : "s");
        else
            hist_add("%s: skipped", irdb_button_label(fnd.ask));
        if(!irdb_find_presses()) fnd.ask = IRDB_BTN_OTHER;
        else finder_next();
    }
    if((down & KEY_Y) && irdb_find_presses()) show_matches();
    if(down & KEY_B) app_back();
}

static void finder_draw(void) {
    char right[48], s[256];
    size_t left = irdb_find_presses() ? finder_left() : irdb_detectable();
    snprintf(right, sizeof(right), "%zu remote%s", left, left == 1 ? "" : "s");
    ui_header("Find my remote", right);
    if(fnd.ask == IRDB_BTN_OTHER)
        snprintf(s, sizeof(s), "Press " BTN_A ", then press any button on your remote.");
    else
        snprintf(s, sizeof(s), "Press " BTN_A ", then press %s on your remote.", irdb_button_label(fnd.ask));
    ui_text_wrap(12, 32, UI_W - 24, S_BODY, ui_col[COL_ACCENT], s);
    float y = 62;
    if(!irdb_find_presses() && !fnd.nhist) {
        ui_text_wrap(12, y, UI_W - 24, S_SMALL, ui_col[COL_TEXT],
                     "A few presses pick your remote out of the database: each button asked for is the one that tells "
                     "the remaining remotes apart best.\n\nHold the remote a few cm from the IR port on the back of "
                     "the 3DS. Remotes with 36 kHz codes (RC5 / RC6, most Philips) can't be recognized.");
    }
    for(int i = 0; i < fnd.nhist; i++, y += SMALL_H + 2) ui_text(12, y, S_SMALL, ui_col[COL_DIM], "%s", fnd.hist[i]);
    if(irdb_find_presses())
        ui_footer(BTN_A " listen   " BTN_X " don't have it   " BTN_Y " matches   " BTN_B " back");
    else if(fnd.ask != IRDB_BTN_OTHER)
        ui_footer(BTN_A " listen   " BTN_X " no such button   " BTN_B " back");
    else
        ui_footer(BTN_A " listen   " BTN_B " back");
}

static const Screen finder_screen = {NULL, finder_input, finder_draw, NULL};

// "irdb:/TVs/LG/LG_TV.ir" -> "TVs/LG/LG_TV", "sdmc:/3ds/3dsir/TVs/Mine.ir" -> "TVs/Mine"
static void db_name(const char* path, char* out, size_t max) {
    const char* p = path;
    if(!strncmp(p, ROOT_DIR "/", strlen(ROOT_DIR) + 1)) p += strlen(ROOT_DIR) + 1;
    else if(irdb_is_packed(p)) p += strlen(IRDB_PACKED) + 1;
    snprintf(out, max, "%s", p);
    size_t n = strlen(out);
    if(n > 3 && !strcasecmp(out + n - 3, ".ir")) out[n - 3] = 0;
}

static char copy_from[512];

static void copy_named(const char* name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ir", ROOT_DIR, name);
    if(bad_file_name(name) || file_exists(path)) {
        toast(bad_file_name(name) ? "Not a usable file name" : "%s.ir exists already", name);
        entry_text("Save a copy", "The copy's name in Remotes (its file is <name>.ir).", name, 60, copy_named);
        return;
    }
    IrFile f;
    if(!irdb_load(copy_from, &f)) {
        msg_box("Can't open", "%s", copy_from);
        return;
    }
    bool ok = irfile_save(&f, path);
    irfile_free(&f);
    if(!ok) {
        msg_box("Couldn't save", "Writing %s failed.", path);
        return;
    }
    irdb_touched(path);
    open_remote(path);
}

static void save_copy(const char* from) {
    snprintf(copy_from, sizeof(copy_from), "%s", from);
    char name[64];
    remote_name(copy_from, name, sizeof(name));
    entry_text("Save a copy", "The copy's name in Remotes (its file is <name>.ir).", name, 60, copy_named);
}

static void matches_input(u32 down, u32 repeat) {
    const IrdbFile* const* c;
    size_t n = irdb_find_candidates(&c);
    ui_list_nav(&fnd.l, n, LIST_ROWS - 2, repeat);
    if(down & KEY_B) {
        app_back();
        return;
    }
    if(!n) return;
    if(down & KEY_A) open_remote(c[fnd.l.sel]->path);
    if(down & KEY_X) save_copy(c[fnd.l.sel]->path);
}

static void matches_draw(void) {
    const IrdbFile* const* c;
    size_t n = irdb_find_candidates(&c);
    char right[32];
    snprintf(right, sizeof(right), "%zu match%s", n, n == 1 ? "" : "es");
    ui_header("Find my remote", right);
    ui_text_wrap(12, LIST_Y, UI_W - 24, S_SMALL, ui_col[COL_DIM],
                 n > 1 ? "These send every code you pressed, best first. Open one to try its buttons."
                       : "This one sends every code you pressed. Open it to try its buttons.");
    float top = LIST_Y + 2 * SMALL_H + 4;
    for(int i = fnd.l.top; i < (int)n && i < fnd.l.top + LIST_ROWS - 2; i++) {
        float y = top + (i - fnd.l.top) * LINE_H;
        bool sel = i == fnd.l.sel;
        if(sel) ui_sel(4, y - 1, UI_W - 8, LINE_H);
        char name[256];
        db_name(c[i]->path, name, sizeof(name));
        ui_text_right(UI_W - 12, y, S_SMALL, ui_col[COL_DIM], "%u buttons", c[i]->buttons);
        ui_text(12, y, S_BODY, ui_col[sel ? COL_TEXT : COL_DIM], "%s", ui_fit(name, UI_W - 100, S_BODY));
    }
    ui_footer(BTN_A " open   " BTN_X " save a copy to Remotes   " BTN_B " back");
}

static const Screen matches_screen = {NULL, matches_input, matches_draw, NULL};

static void finder_begin(void) {
    irdb_find_reset();
    memset(&fnd, 0, sizeof(fnd));
    fnd.ask = irdb_button_id("Power");
    app_screen(&finder_screen);
}

static void finder_loaded(void) {
    if(!dbj.ok) return; // stopped while indexing
    if(irdb_detectable()) {
        finder_begin();
        return;
    }
    confirm("Find my remote", db_check,
            "There are no remotes to compare with yet. Download the Flipper-IRDB (about 18 MB, ~9000 remotes) "
            "over Wi-Fi?");
}

static void open_finder(void) {
    mkdir(ROOT_DIR, 0777);
    job_start("Find my remote", db_refresh_run, finder_loaded, JOB_CANCEL | JOB_CPU);
}

// ---------------------------------------------------------------- the browser

static struct {
    char path[256];
    char** names; // "name/" for folders
    int n;
    UiList l;
} br;

static int cmp_str(const void* a, const void* b) {
    return strcasecmp(*(char* const*)a, *(char* const*)b);
}

// The downloaded database, a folder in Remotes that is really the pack (irdb.h): its path is IRDB_PACKED.
#define VIRT_NAME "Flipper-IRDB/"

static bool br_virtual(void) {
    return !strncmp(br.path, IRDB_PACKED, strlen(IRDB_PACKED));
}

static void br_name_add(int* cap, const char* name) {
    if(br.n == *cap) br.names = realloc(br.names, (*cap = *cap ? *cap * 2 : 32) * sizeof(char*));
    br.names[br.n++] = strdup(name);
}

// A folder of the pack: the index's paths under it, a level deep.
static void br_scan_virtual(int* cap) {
    char prefix[300], last[256] = "";
    snprintf(prefix, sizeof(prefix), "%s/", br.path);
    size_t pl = strlen(prefix);
    for(size_t i = 0; i < irdb_files(); i++) {
        const char* p = irdb_file(i)->path;
        if(strncmp(p, prefix, pl)) continue;
        p += pl;
        const char* slash = strchr(p, '/');
        char name[256];
        if(slash) snprintf(name, sizeof(name), "%.*s/", (int)(slash - p), p);
        else snprintf(name, sizeof(name), "%s", p);
        if(slash && !strcmp(name, last)) continue; // the folder's other files
        if(slash) snprintf(last, sizeof(last), "%s", name);
        br_name_add(cap, name);
    }
}

static void br_scan(void) {
    for(int i = 0; i < br.n; i++) free(br.names[i]);
    free(br.names);
    br.names = NULL;
    br.n = 0;
    int cap = 0;
    if(br_virtual()) {
        br_scan_virtual(&cap);
        qsort(br.names, br.n, sizeof(char*), cmp_str);
        ui_list_fix(&br.l, br.n, LIST_ROWS);
        return;
    }
    if(!strcmp(br.path, ROOT_DIR) && file_exists(IRDB_PACK)) br_name_add(&cap, VIRT_NAME);
    DIR* d = opendir(br.path);
    struct dirent* e;
    while(d && (e = readdir(d))) {
        if(e->d_name[0] == '.') continue;
        bool dir = e->d_type == DT_DIR;
        size_t len = strlen(e->d_name);
        if(!dir && (len < 3 || strcasecmp(e->d_name + len - 3, ".ir"))) continue;
        char name[260];
        snprintf(name, sizeof(name), "%s%s", e->d_name, dir ? "/" : "");
        if(!strcmp(br.path, ROOT_DIR) && !strcmp(name, VIRT_NAME)) continue; // a real one is hidden by the pack
        br_name_add(&cap, name);
    }
    if(d) closedir(d);
    qsort(br.names, br.n, sizeof(char*), cmp_str);
    ui_list_fix(&br.l, br.n, LIST_ROWS);
}

static void br_show(void) {
    br_scan(); // a remote may have been made or renamed since
}

static void br_sel_path(char* out, size_t max) {
    snprintf(out, max, "%.255s/%s", br.path, br.names[br.l.sel]);
    size_t n = strlen(out);
    if(out[n - 1] == '/') out[n - 1] = 0;
}

// Into the pack's folder: the index lists it, and is loaded first if it isn't.
static void br_enter_virtual(void) {
    snprintf(br.path, sizeof(br.path), "%s", IRDB_PACKED);
    br.l = (UiList){0};
    br_scan();
}

static void br_virtual_loaded(void) {
    if(dbj.ok) br_enter_virtual();
}

static bool br_sel_dir(void) {
    const char* s = br.names[br.l.sel];
    return s[strlen(s) - 1] == '/';
}

static void br_renamed(const char* name) {
    char from[512], to[512];
    br_sel_path(from, sizeof(from));
    snprintf(to, sizeof(to), "%s/%s.ir", br.path, name);
    if(bad_file_name(name)) {
        toast("Not a usable file name");
    } else if(file_exists(to)) {
        toast("%s.ir exists already", name);
    } else if(rename(from, to)) {
        msg_box("Rename", "Renaming failed (%d).", errno);
    } else {
        irdb_touched(from);
        irdb_touched(to);
        toast("Renamed");
    }
    br_scan();
}

static void br_rename(void) {
    char name[256];
    remote_name(br.names[br.l.sel], name, sizeof(name));
    entry_text("Rename remote", "The remote's new name (its file is <name>.ir).", name, 60, br_renamed);
}

static void br_delete_yes(void) {
    char path[512];
    br_sel_path(path, sizeof(path));
    if(remove(path)) {
        msg_box("Delete", "Deleting failed (%d).", errno);
    } else {
        irdb_touched(path);
        toast("Deleted");
    }
    br_scan();
}

static void br_delete(void) {
    char name[256];
    remote_name(br.names[br.l.sel], name, sizeof(name));
    confirm("Delete remote", br_delete_yes, "Delete the remote \"%s\" and all its buttons?", name);
}

static void br_created(const char* name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ir", br.path, name);
    if(bad_file_name(name) || file_exists(path)) {
        toast(bad_file_name(name) ? "Not a usable file name" : "%s.ir exists already", name);
        return;
    }
    IrFile f;
    irfile_init(&f);
    if(!irfile_save(&f, path)) {
        msg_box("Couldn't save", "Writing %s failed.", path);
        return;
    }
    irdb_touched(path);
    open_remote(path);
}

static void br_new_empty(void) {
    entry_text("New remote", "The new remote's name (its file is <name>.ir). Then learn its buttons.", "", 60, br_created);
}

static void br_new_learn(void) {
    learn_new_remote(br.path);
}

static void br_input(u32 down, u32 repeat) {
    ui_list_nav(&br.l, br.n, LIST_ROWS, repeat);
    if(down & KEY_B) {
        if(!strcmp(br.path, ROOT_DIR)) {
            app_back();
            return;
        }
        if(!strcmp(br.path, IRDB_PACKED)) { // out of the pack, onto it
            snprintf(br.path, sizeof(br.path), "%s", ROOT_DIR);
            br_scan();
            for(int i = 0; i < br.n; i++)
                if(!strcmp(br.names[i], VIRT_NAME)) br.l.sel = i;
            ui_list_fix(&br.l, br.n, LIST_ROWS);
            return;
        }
        // up a folder, the cursor on the one we came from
        char from[260];
        snprintf(from, sizeof(from), "%s/", base_name(br.path));
        *strrchr(br.path, '/') = 0;
        br_scan();
        for(int i = 0; i < br.n; i++)
            if(!strcmp(br.names[i], from)) br.l.sel = i;
        ui_list_fix(&br.l, br.n, LIST_ROWS);
        return;
    }
    if((down & KEY_Y) && !br_virtual()) {
        static const MenuItem items[] = {{"Learn its first button", br_new_learn}, {"Empty remote", br_new_empty}};
        popup("New remote", items, 2);
    }
    if(!br.n) return;
    if(down & KEY_A) {
        char path[512];
        br_sel_path(path, sizeof(path));
        if(!strcmp(br.path, ROOT_DIR) && !strcmp(br.names[br.l.sel], VIRT_NAME)) {
            if(irdb_loaded()) br_enter_virtual();
            else job_start("Flipper-IRDB", db_refresh_run, br_virtual_loaded, JOB_CANCEL | JOB_CPU);
        } else if(br_sel_dir()) {
            snprintf(br.path, sizeof(br.path), "%.255s", path);
            br.l = (UiList){0};
            br_scan();
        } else {
            open_remote(path);
        }
    }
    if((down & KEY_X) && br_virtual() && !br_sel_dir()) {
        char path[512];
        br_sel_path(path, sizeof(path));
        save_copy(path);
    } else if((down & KEY_X) && !br_sel_dir()) {
        static const MenuItem items[] = {{"Rename", br_rename}, {"Delete", br_delete}};
        char name[64];
        remote_name(br.names[br.l.sel], name, sizeof(name));
        popup(name, items, 2);
    }
}

static void br_draw(void) {
    char title[288], right[32];
    if(br_virtual()) snprintf(title, sizeof(title), "Remotes/Flipper-IRDB%s", br.path + strlen(IRDB_PACKED));
    else snprintf(title, sizeof(title), "Remotes%s", br.path + strlen(ROOT_DIR));
    snprintf(right, sizeof(right), "%d item%s", br.n, br.n == 1 ? "" : "s");
    ui_header(title, right);
    if(!br.n)
        ui_text_wrap(12, 40, UI_W - 24, S_BODY, ui_col[COL_DIM],
                     "No remotes here yet.\n\n" BTN_Y " makes one. Flipper Zero .ir files copied to " ROOT_DIR
                     " show up here too (folders work: the whole Flipper-IRDB fits).");
    for(int i = br.l.top; i < br.n && i < br.l.top + LIST_ROWS; i++) {
        float y = LIST_Y + (i - br.l.top) * LINE_H;
        bool sel = i == br.l.sel, dir = br.names[i][strlen(br.names[i]) - 1] == '/';
        if(sel) ui_sel(4, y - 1, UI_W - 8, LINE_H);
        char name[256];
        if(dir) snprintf(name, sizeof(name), "%s", br.names[i]);
        else remote_name(br.names[i], name, sizeof(name));
        ui_text(12, y, S_BODY, ui_col[dir ? COL_ACCENT : sel ? COL_TEXT : COL_DIM], "%s", ui_fit(name, UI_W - 30, S_BODY));
    }
    if(br_virtual()) ui_footer(BTN_A " open   " BTN_X " save a copy   " BTN_B " back");
    else ui_footer(BTN_A " open   " BTN_X " edit   " BTN_Y " new remote   " BTN_B " back");
}

static const Screen browser_screen = {br_show, br_input, br_draw, NULL};

static void open_browser(void) {
    mkdir(ROOT_DIR, 0777);
    snprintf(br.path, sizeof(br.path), "%s", ROOT_DIR);
    br.l = (UiList){0};
    app_screen(&browser_screen);
}

// ---------------------------------------------------------------- settings

static UiList set_list;

static void set_input(u32 down, u32 repeat) {
    ui_list_nav(&set_list, 2, 2, repeat & (KEY_UP | KEY_DOWN));
    bool left = repeat & KEY_LEFT, right = repeat & (KEY_RIGHT | KEY_A);
    if(set_list.sel == 0 && (down & (KEY_LEFT | KEY_RIGHT | KEY_A))) {
        style = style == IrCarrierClean ? IrCarrierDense : IrCarrierClean;
        settings_save();
    }
    if(set_list.sel == 1 && (left || right)) {
        if(left && repeats > 1) repeats--;
        if(right && repeats < 10) repeats++;
        settings_save();
    }
    if(down & KEY_B) app_back();
}

static void set_draw(void) {
    ui_header("Settings", NULL);
    static const char* const help[2] = {
        "Dense: 0x00 bytes at 36000 / 38400 baud, one light pulse per UART bit. The strongest signal, tested on TVs "
        "across a room, but every tenth pulse of the carrier is missing.\n\nClean: 0x55 bytes at twice the rate. A "
        "gapless carrier (what a Flipper Zero right next to the port decodes), but about half as strong.",
        "How many times each press sends its code, back to back. Some receivers (Denon) ignore a single frame. "
        "Also " BTN_L " / " BTN_R " on a remote's screen.",
    };
    char v[2][32];
    snprintf(v[0], sizeof(v[0]), "%s", style == IrCarrierClean ? "clean" : "dense");
    snprintf(v[1], sizeof(v[1]), "%u", repeats);
    const char* label[2] = {"Carrier", "Sends per press"};
    for(int i = 0; i < 2; i++) {
        float y = 30 + i * 20;
        if(i == set_list.sel) ui_sel(8, y - 3, UI_W - 16, 19);
        ui_text(20, y, S_BODY, ui_col[i == set_list.sel ? COL_TEXT : COL_DIM], "%s", label[i]);
        ui_text_right(UI_W - 20, y, S_BODY, ui_col[COL_ACCENT], " %s ", v[i]);
    }
    ui_rect(8, 76, UI_W - 16, 130, ui_col[COL_PANEL]);
    ui_text_wrap(16, 82, UI_W - 32, S_SMALL, ui_col[COL_TEXT], help[set_list.sel]);
    ui_footer(BTN_UD " setting   " BTN_LR " change   " BTN_B " back");
}

static const Screen settings_screen = {NULL, set_input, set_draw, NULL};

static void open_settings(void) {
    set_list = (UiList){0};
    app_screen(&settings_screen);
}

// ---------------------------------------------------------------- tools

static void carrier_run(void) {
    static u8 buf[0x800];
    const IrCarrier* c = ir_pick_carrier(38000, style);
    memset(buf, c->cycle_slots == 2 ? 0x55 : 0x00, sizeof(buf));
    logp("carrier %lu Hz, %s\n", c->baud / c->cycle_slots, style == IrCarrierClean ? "clean" : "dense");
    while(!job_cancel) {
        if(ui_keys_held & KEY_A) {
            job_note("Sending a %lu Hz carrier.", c->baud / c->cycle_slots);
            Result rc = app_send_bytes(c, buf, sizeof(buf));
            if(R_FAILED(rc)) {
                logp("send: %08lX\n", rc);
                svcSleepThread(MS(500));
            }
        } else {
            job_note("Hold " BTN_A " to send a 38 kHz carrier, and look at the IR port (on the back) through a phone "
                     "camera: it lights up purple.");
            svcSleepThread(MS(16));
        }
    }
}

static void tool_carrier(void) {
    job_start("Carrier test", carrier_run, NULL, JOB_CANCEL);
}

static void timing_run(void) {
    job_note("Sends at every ir:u bit rate and times it: the measured rate should match the nominal one.");
    app_timing_check(&job_cancel);
}

static void timing_done(void) {
    app_open_log();
}

static void tool_timing(void) {
    job_start("Bit rate / timing check", timing_run, timing_done, JOB_CANCEL);
}

#ifndef APP_RELEASE
// Takes the SC16IS750 over (shares the ir module's session), logs its registers and the I2C round-trip
// time, then counts what the RX FIFO receives until B: point a remote at the port.
static void diag_run(void) {
    Result rc = app_hw_direct(IRHW_STEAL);
    if(R_SUCCEEDED(rc)) {
        irhw_dump();
        u32 mn, avg, mx;
        rc = irhw_bench(200, &mn, &avg, &mx);
        logp("I2C reg read: min %lu avg %lu max %lu us (%08lX)\n", mn, avg, mx, rc);
    } else {
        logp("takeover failed: %08lX\n", rc);
    }
    job_note("Point a remote at the port: received bytes are counted. " BTN_B " gives the hardware back.");
    u32 bytes = 0, polls = 0;
    u64 t0 = svcGetSystemTick();
    while(!job_cancel && irhw_owned()) {
        u8 lvl = 0, buf[64];
        if(R_SUCCEEDED(sc_get(SC_RXLVL, &lvl)) && lvl) {
            if(lvl > 64) lvl = 64;
            sc_read(SC_RHR, buf, lvl);
            bytes += lvl;
        }
        polls++;
        if(irhw_us(svcGetSystemTick() - t0) >= 500000) {
            if(bytes) logp("RX %lu bytes in 0.5 s (%lu polls)\n", bytes, polls);
            bytes = polls = 0;
            t0 = svcGetSystemTick();
        }
    }
    app_hw_iru();
    logp("ir module back\n");
}

static void tool_diag(void) {
    job_start("IR chip diagnostic", diag_run, NULL, JOB_CANCEL);
}
#endif

static const MenuEntry tool_entries[] = {
    {"Carrier test", "Sends a 38 kHz carrier while " BTN_A " is held, to see the IR port light up through a phone camera "
                     "(front cameras usually have no IR filter).", tool_carrier},
    {"Bit rate check", "Times a fixed-size send at every ir:u bit rate. The measured rate should match the nominal one; "
                       "if it's lower, ir:u leaves gaps between bytes.", tool_timing},
#ifndef APP_RELEASE
    {"IR chip diagnostic", "Takes the SC16IS750 over (sharing the ir module's I2C session), logs its registers and the "
                           "I2C round trip, then counts the bytes its receiver gets.", tool_diag},
#endif
    {"Log", "Everything the app logged this session.", app_open_log},
};
static Menu tool_menu = {"Tools", tool_entries, sizeof(tool_entries) / sizeof(tool_entries[0]), {0}};

static void tool_input(u32 down, u32 repeat) {
    menu_input(&tool_menu, down, repeat);
}

static void tool_draw(void) {
    menu_draw(&tool_menu);
}

static const Screen tool_screen = {NULL, tool_input, tool_draw, NULL};

static void open_tools(void) {
    app_screen(&tool_screen);
}

// ---------------------------------------------------------------- root

static void root_learn(void) {
    mkdir(ROOT_DIR, 0777);
    learn_new_remote(ROOT_DIR);
}

static const MenuEntry root_entries[] = {
    {"Remotes", "Your remotes: Flipper Zero .ir files in " ROOT_DIR " (folders work, so the whole Flipper-IRDB can go "
                "there). Open one to send its buttons, or learn, rename, move and delete them.", open_browser},
    {"Find my remote", "Press a few buttons on your remote, as asked, and 3DSir finds it among the Flipper-IRDB's "
                       "thousands of remotes (and your own .ir files), with all its buttons.\n\nThe first time, it "
                       "offers to download the Flipper-IRDB over Wi-Fi.", open_finder},
    {"Learn new remote", "Point a remote at the IR port on the back of the 3DS and press a button: it becomes the "
                         "first button of a new remote.\n\nLearns NEC, Samsung, Sony, Kaseikyo (Panasonic), Pioneer and "
                         "RCA codes. RC5 / RC6 (Philips) can't be learned.", root_learn},
    {"Built-in test codes", "Power and Vol+ for Samsung, LG, Sony and Philips TVs, to check the 3DS reaches your TV.",
     open_builtin},
    {"Remote database", "Download or update the Flipper-IRDB over Wi-Fi, or index the remotes on the SD card again.",
     open_db},
    {"Settings", "The carrier style (dense: strongest, clean: what a Flipper decodes) and how many times each press "
                 "sends its code.", open_settings},
    {"Tools", "Carrier test, bit rate check, the log.", open_tools},
    {"Exit", "Back to the HOME menu.", app_quit},
};
static Menu root_menu = {"3DSir", root_entries, sizeof(root_entries) / sizeof(root_entries[0]), {0}};

static void root_input(u32 down, u32 repeat) {
    menu_input(&root_menu, down, repeat);
}

static void root_draw(void) {
    menu_draw(&root_menu);
    if(!iru_is_open()) ui_text(12, 196, S_SMALL, ui_col[COL_ERR], "ir:u is unavailable: nothing can be sent.");
}

static const Screen root_screen = {NULL, root_input, root_draw, NULL};

const Screen* screens_root(void) {
    return &root_screen;
}
