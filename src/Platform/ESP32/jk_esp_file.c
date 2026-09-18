#include "jk_esp_file.h"
#include "jk_esp_fs.h"
#include "jk_esp.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// use the real libc calls here
#undef fopen
#undef stat
#undef lstat
#undef opendir
#undef scandir
#undef mkdir
#undef rmdir
#undef unlink
#undef remove
#undef getcwd
#undef chdir

uint64_t jk_esp_file_usLoad = 0, jk_esp_file_usDirect = 0, jk_esp_file_usOpen = 0;
uint32_t jk_esp_file_nLoad = 0, jk_esp_file_nDirect = 0, jk_esp_file_nOpen = 0, jk_esp_file_nOpenFail = 0;

#define JK_WIN_BYTES (16 * 1024)
#define JK_WIN_COUNT 4

typedef struct {
    int fd;
    int writable;
    long size;
    long pos;
    uint8_t* win[JK_WIN_COUNT];
    long winOff[JK_WIN_COUNT];  // -1 = empty
    long winLen[JK_WIN_COUNT];
    uint32_t winUse[JK_WIN_COUNT];
    uint32_t useCounter;
} jk_esp_file_t;

static const char* jk_esp_file_resolve(const char* fpath, char* tmp, size_t tmplen)
{
    // backslashes -> slashes, relative -> game dir
    size_t n = strlen(fpath);
    if (n >= tmplen) n = tmplen - 1;
    for (size_t i = 0; i < n; i++) tmp[i] = fpath[i] == '\\' ? '/' : fpath[i];
    tmp[n] = 0;
    return jk_esp_fs_path(tmp);
}

stdFile_t jk_esp_file_open(const char* fpath, const char* mode)
{
    char tmp[512];
    const char* path = jk_esp_file_resolve(fpath, tmp, sizeof(tmp));
    int writable = strchr(mode, 'w') != NULL || strchr(mode, 'a') != NULL || strchr(mode, '+') != NULL;
    int flags;
    if (strchr(mode, 'w')) flags = (strchr(mode, '+') ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
    else if (strchr(mode, 'a')) flags = (strchr(mode, '+') ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
    else flags = strchr(mode, '+') ? O_RDWR : O_RDONLY;

    if (!writable && !jk_esp_fs_dir_might_exist(path)) {
        return 0;
    }
    uint64_t t0 = jk_esp_time_us();
    int fd = open(path, flags, 0666);
    jk_esp_file_usOpen += jk_esp_time_us() - t0; jk_esp_file_nOpen++;
    if (fd < 0) {
        jk_esp_file_nOpenFail++;
        return 0;
    }
    jk_esp_file_t* f = (jk_esp_file_t*)calloc(1, sizeof(jk_esp_file_t));
    if (!f) { close(fd); return 0; }
    f->fd = fd;
    f->writable = writable;
    struct stat st;
    f->size = (fstat(fd, &st) == 0) ? (long)st.st_size : 0;
    f->pos = strchr(mode, 'a') ? f->size : 0;
    for (int i = 0; i < JK_WIN_COUNT; i++) f->winOff[i] = -1;
    return (stdFile_t)f;
}

int jk_esp_file_close(stdFile_t h)
{
    jk_esp_file_t* f = (jk_esp_file_t*)h;
    if (!f) return -1;
    for (int i = 0; i < JK_WIN_COUNT; i++) free(f->win[i]);
    int ret = close(f->fd);
    free(f);
    return ret;
}

// Return a pointer to the cached bytes at f->pos and how many are contiguous
// there, loading a window if needed. Returns 0 bytes at EOF / on error.
static size_t jk_esp_file_window(jk_esp_file_t* f, const uint8_t** out)
{
    if (f->pos >= f->size) return 0;
    for (int i = 0; i < JK_WIN_COUNT; i++) {
        if (f->winOff[i] >= 0 && f->pos >= f->winOff[i] && f->pos < f->winOff[i] + f->winLen[i]) {
            f->winUse[i] = ++f->useCounter;
            *out = f->win[i] + (f->pos - f->winOff[i]);
            return (size_t)(f->winOff[i] + f->winLen[i] - f->pos);
        }
    }
    // pick the least recently used window
    int slot = 0;
    for (int i = 1; i < JK_WIN_COUNT; i++) {
        if (f->winOff[i] < 0) { slot = i; break; }
        if (f->winUse[i] < f->winUse[slot]) slot = i;
    }
    if (!f->win[slot]) {
        f->win[slot] = (uint8_t*)jk_esp_malloc(JK_WIN_BYTES);
        if (!f->win[slot]) return 0;
    }
    long off = f->pos & ~(long)(JK_WIN_BYTES - 1);
    uint64_t t0 = jk_esp_time_us();
    if (lseek(f->fd, off, SEEK_SET) != off) return 0;
    ssize_t n = read(f->fd, f->win[slot], JK_WIN_BYTES);
    jk_esp_file_usLoad += jk_esp_time_us() - t0; jk_esp_file_nLoad++;
    if (n <= 0) { f->winOff[slot] = -1; return 0; }
    f->winOff[slot] = off;
    f->winLen[slot] = (long)n;
    f->winUse[slot] = ++f->useCounter;
    if (f->pos >= off + n) return 0;
    *out = f->win[slot] + (f->pos - off);
    return (size_t)(off + n - f->pos);
}

size_t jk_esp_file_read(stdFile_t h, void* dst, size_t len)
{
    jk_esp_file_t* f = (jk_esp_file_t*)h;
    if (!f || !dst || !len) return 0;
    if (f->writable) {
        lseek(f->fd, f->pos, SEEK_SET);
        ssize_t n = read(f->fd, dst, len);
        if (n < 0) n = 0;
        f->pos += n;
        return (size_t)n;
    }
    // big reads bypass the cache
    if (len >= JK_WIN_BYTES) {
        uint64_t t0 = jk_esp_time_us();
        lseek(f->fd, f->pos, SEEK_SET);
        ssize_t n = read(f->fd, dst, len);
        jk_esp_file_usDirect += jk_esp_time_us() - t0; jk_esp_file_nDirect++;
        if (n < 0) n = 0;
        f->pos += n;
        return (size_t)n;
    }
    size_t done = 0;
    uint8_t* d = (uint8_t*)dst;
    while (done < len) {
        const uint8_t* src;
        size_t avail = jk_esp_file_window(f, &src);
        if (!avail) break;
        size_t n = len - done < avail ? len - done : avail;
        memcpy(d + done, src, n);
        done += n;
        f->pos += n;
    }
    return done;
}

size_t jk_esp_file_write(stdFile_t h, void* src, size_t len)
{
    jk_esp_file_t* f = (jk_esp_file_t*)h;
    if (!f || !src || !len) return 0;
    lseek(f->fd, f->pos, SEEK_SET);
    ssize_t n = write(f->fd, src, len);
    if (n < 0) n = 0;
    f->pos += n;
    if (f->pos > f->size) f->size = f->pos;
    for (int i = 0; i < JK_WIN_COUNT; i++) f->winOff[i] = -1;
    return (size_t)n;
}

const char* jk_esp_file_gets(stdFile_t h, char* dst, size_t len)
{
    jk_esp_file_t* f = (jk_esp_file_t*)h;
    if (!f || !dst || len < 2) return NULL;
    size_t i = 0;
    while (i < len - 1) {
        const uint8_t* src;
        size_t avail = jk_esp_file_window(f, &src);
        if (!avail) break;
        size_t want = len - 1 - i;
        size_t n = avail < want ? avail : want;
        const uint8_t* nl = (const uint8_t*)memchr(src, '\n', n);
        if (nl) n = (size_t)(nl - src) + 1;
        memcpy(dst + i, src, n);
        i += n;
        f->pos += n;
        if (nl) break;
    }
    if (i == 0) return NULL;
    dst[i] = 0;
    return dst;
}

const char16_t* jk_esp_file_getws(stdFile_t h, char16_t* dst, size_t len)
{
    jk_esp_file_t* f = (jk_esp_file_t*)h;
    if (!f || !len) return NULL;
    size_t i = 0;
    while (i < len - 1) {
        char16_t ch = 0;
        if (jk_esp_file_read(h, &ch, sizeof(ch)) != sizeof(ch)) {
            if (i == 0) return NULL;
            break;
        }
        dst[i++] = ch;
        if (ch == u'\n') break;
    }
    dst[i] = u'\0';
    return dst;
}

int jk_esp_file_eof(stdFile_t h)
{
    jk_esp_file_t* f = (jk_esp_file_t*)h;
    if (!f) return 1;
    return f->pos >= f->size;
}

int jk_esp_file_tell(stdFile_t h)
{
    jk_esp_file_t* f = (jk_esp_file_t*)h;
    return f ? (int)f->pos : 0;
}

int jk_esp_file_seek(stdFile_t h, int offset, int whence)
{
    jk_esp_file_t* f = (jk_esp_file_t*)h;
    if (!f) return -1;
    long target;
    switch (whence) {
    case SEEK_SET: target = offset; break;
    case SEEK_CUR: target = f->pos + offset; break;
    case SEEK_END: target = f->size + offset; break;
    default: return -1;
    }
    if (target < 0) target = 0;
    f->pos = target;
    return 0;
}

int jk_esp_file_size(stdFile_t h)
{
    jk_esp_file_t* f = (jk_esp_file_t*)h;
    return f ? (int)f->size : 0;
}

int jk_esp_file_printf(stdFile_t h, const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) return n;
    if ((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;
    return (int)jk_esp_file_write(h, buf, (size_t)n);
}
