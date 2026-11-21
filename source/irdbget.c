// Downloading the Flipper-IRDB over Wi-Fi: the newest commit's tarball from GitHub (libcurl + mbedtls, the
// CA bundle built in), unpacked with libarchive into one pack file (irdb.c), its .ir files only, each indexed
// as it goes in. The new pack replaces the old one only when complete.

#include <3ds.h>
#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app.h"
#include "cacert_pem.h"
#include "irdb.h"
#include "log.h"

#define REPO "Lucaslhm/Flipper-IRDB"
#define TARBALL ROOT_DIR "/.irdb-%.40s.tar.gz" // the commit; kept until it's unpacked, so a retry needn't
                                              // download it again
#define MAX_FILE (4 << 20) // a .ir file larger than this isn't a remote

static void fail(char* err, size_t max, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static void fail(char* err, size_t max, const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vsnprintf(err, max, fmt, va);
    va_end(va);
    logp("irdb: %s\n", err);
}

// ---------------------------------------------------------------- network

static bool net_up;
static char* ca; // the CA bundle, NUL-terminated for mbedtls

static bool net_begin(char* err, size_t max) {
    if(!soc_acquire()) {
        fail(err, max, "The network isn't available. Is Wi-Fi on?");
        return false;
    }
    sslcInit(0); // mbedtls takes its entropy from sslcGenerateRandomData
    static bool once;
    if(!once) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        once = true;
    }
    if(!ca) {
        ca = malloc(cacert_pem_size + 1);
        memcpy(ca, cacert_pem, cacert_pem_size);
        ca[cacert_pem_size] = 0;
    }
    net_up = true;
    return true;
}

static void net_end(void) {
    if(!net_up) return;
    sslcExit();
    soc_release();
    net_up = false;
}

typedef struct {
    volatile bool* cancel;
    FILE* fp; // the download goes here, or:
    char* mem; // here (up to cap)
    size_t len, cap;
} Sink;

static size_t on_data(char* p, size_t size, size_t n, void* user) {
    Sink* s = user;
    size_t len = size * n;
    if(s->fp) return fwrite(p, 1, len, s->fp);
    if(s->len + len > s->cap) return 0;
    memcpy(s->mem + s->len, p, len);
    s->len += len;
    return len;
}

static int on_progress(void* user, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un) {
    (void)ut, (void)un;
    Sink* s = user;
    if(s->fp) {
        if(total > 0) job_note("Downloading the Flipper-IRDB: %.1f of %.1f MB", now / 1048576.0, total / 1048576.0);
        else job_note("Downloading the Flipper-IRDB: %.1f MB", now / 1048576.0);
    }
    return s->cancel && *s->cancel;
}

static bool fetch(const char* url, const char* accept, Sink* s, char* err, size_t max) {
    CURL* c = curl_easy_init();
    if(!c) {
        fail(err, max, "curl_easy_init failed");
        return false;
    }
    struct curl_blob blob = {ca, cacert_pem_size + 1, CURL_BLOB_NOCOPY};
    struct curl_slist* hdr = NULL;
    if(accept) hdr = curl_slist_append(hdr, accept);
    char cerr[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "3DSir");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &blob);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 256L); // bytes/s, for:
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 64L * 1024);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, s);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, s);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, cerr);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    curl_slist_free_all(hdr);
    if(rc == CURLE_OK) return true;
    if(rc == CURLE_ABORTED_BY_CALLBACK) fail(err, max, "Stopped.");
    else fail(err, max, "Download failed: %s", *cerr ? cerr : curl_easy_strerror(rc));
    return false;
}

bool irdb_latest(char* sha, size_t max, char* err, size_t errmax) {
    if(!net_begin(err, errmax)) return false;
    char buf[64];
    Sink s = {.mem = buf, .cap = sizeof(buf) - 1};
    job_note("Asking GitHub for the newest Flipper-IRDB...");
    bool ok = fetch("https://api.github.com/repos/" REPO "/commits/main", "Accept: application/vnd.github.sha", &s, err,
                    errmax);
    net_end();
    if(!ok) return false;
    buf[s.len] = 0;
    buf[strcspn(buf, "\r\n")] = 0;
    if(strlen(buf) != 40 || strspn(buf, "0123456789abcdef") != 40) {
        fail(err, errmax, "GitHub sent something unexpected.");
        return false;
    }
    snprintf(sha, max, "%s", buf);
    return true;
}

// ---------------------------------------------------------------- unpacking

static u64 sd_free(void) {
    FS_ArchiveResource r;
    if(R_FAILED(FSUSER_GetSdmcArchiveResource(&r))) return ~0ULL;
    return (u64)r.freeClusters * r.clusterSize;
}

static bool safe_rel(const char* rel) {
    if(!*rel || strstr(rel, "\\")) return false;
    for(const char* p = rel; p; p = strchr(p, '/')) {
        if(*p == '/') p++;
        if(*p == '.' || !*p) return false; // "..", hidden, empty component
    }
    return true;
}

static bool is_ir(const char* name) {
    size_t n = strlen(name);
    return n > 3 && !strcasecmp(name + n - 3, ".ir");
}

// Every .ir file in the tarball into the pack (irdb.c), which indexes it as it goes in.
static bool unpack(const char* tarball, const char* sha, volatile bool* cancel, char* err, size_t max,
                   size_t* count) {
    struct archive* a = archive_read_new();
    archive_read_support_filter_gzip(a);
    archive_read_support_format_tar(a);
    if(archive_read_open_filename(a, tarball, 64 * 1024) != ARCHIVE_OK) {
        fail(err, max, "Can't read the download: %s", archive_error_string(a));
        archive_read_free(a);
        return false;
    }
    if(!irdb_pack_begin(sha)) {
        fail(err, max, "Can't write the remote database. Is the SD card full?");
        archive_read_free(a);
        return false;
    }
    struct stat st;
    u64 total = stat(tarball, &st) ? 0 : st.st_size;
    char* buf = malloc(MAX_FILE);
    struct archive_entry* e;
    bool ok = true;
    int r;
    *count = 0;
    while(ok && (r = archive_read_next_header(a, &e)) == ARCHIVE_OK) {
        if(*cancel) {
            fail(err, max, "Stopped.");
            ok = false;
            break;
        }
        const char* name = archive_entry_pathname(e);
        const char* rel = name ? strchr(name, '/') : NULL; // past "Flipper-IRDB-<sha>/"
        la_int64_t size = archive_entry_size(e);
        if(archive_entry_filetype(e) != AE_IFREG || !rel || !is_ir(++rel) || !safe_rel(rel) || size > MAX_FILE)
            continue;
        if(archive_read_data(a, buf, size) != size) {
            fail(err, max, "The download is damaged (%s).", archive_error_string(a));
            ok = false;
            break;
        }
        if(!irdb_pack_add(rel, buf, size)) {
            fail(err, max, "Writing the remote database failed. Is the SD card full?");
            ok = false;
            break;
        }
        if(++*count % 16 == 0) {
            u64 at = archive_filter_bytes(a, -1);
            job_note("Unpacking the Flipper-IRDB: %zu remotes, %llu%%", *count, total ? at * 100 / total : 0);
        }
    }
    if(ok && r != ARCHIVE_EOF) {
        fail(err, max, "The download is damaged (%s).", archive_error_string(a));
        ok = false;
    }
    if(ok && !*count) {
        fail(err, max, "The download had no remotes in it.");
        ok = false;
    }
    free(buf);
    archive_read_free(a);
    job_note("Saving the remote index...");
    if(!irdb_pack_end(ok) && ok) {
        fail(err, max, "Writing the remote database failed. Is the SD card full?");
        ok = false;
    }
    return ok;
}

bool irdb_install(const char* sha, volatile bool* cancel, char* err, size_t max) {
    char tarball[300], part[310];
    snprintf(tarball, sizeof(tarball), TARBALL, sha);
    snprintf(part, sizeof(part), "%s.part", tarball);
    struct stat st;
    size_t count = 0;
    if(!stat(tarball, &st)) {
        logp("irdb: %s downloaded already\n", tarball);
    } else {
        // 18 MB to download, ~90 MB of pack while the old one is still there.
        u64 need = 200ull << 20;
        if(sd_free() < need) {
            fail(err, max, "The SD card needs about %llu MB free for this.", need >> 20);
            return false;
        }
        if(!net_begin(err, max)) return false;
        FILE* fp = fopen(part, "wb");
        if(!fp) {
            fail(err, max, "Can't write %s.", part);
            net_end();
            return false;
        }
        static char wbuf[64 * 1024];
        setvbuf(fp, wbuf, _IOFBF, sizeof(wbuf));
        Sink s = {.cancel = cancel, .fp = fp};
        char url[160];
        snprintf(url, sizeof(url), "https://codeload.github.com/" REPO "/tar.gz/%s", sha);
        bool ok = fetch(url, NULL, &s, err, max);
        if(fclose(fp) && ok) {
            fail(err, max, "Writing the download failed. Is the SD card full?");
            ok = false;
        }
        net_end();
        if(ok) {
            remove(tarball);
            if(rename(part, tarball)) {
                fail(err, max, "Can't rename the download (%d).", errno);
                ok = false;
            }
        }
        if(!ok) {
            remove(part);
            return false;
        }
    }
    u64 t0 = svcGetSystemTick();
    if(!unpack(tarball, sha, cancel, err, max, &count)) return false; // the download is kept for a retry
    logp("irdb: %zu remotes installed (%s) in %llu s\n", count, sha,
         (svcGetSystemTick() - t0) / SYSCLOCK_ARM11);
    remove(tarball);
    return true;
}
