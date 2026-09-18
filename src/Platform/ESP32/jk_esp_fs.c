#include "jk_esp_fs.h"
#include "jk_esp.h"

#include <string.h>

// jk_esp_fs.h is force-included into this file too; use the real functions.
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

#define JK_ESP_FS_NBUF 4
#define JK_ESP_FS_PATHLEN 512

static char jk_esp_fs_cwd[JK_ESP_FS_PATHLEN] = "";
static char jk_esp_fs_bufs[JK_ESP_FS_NBUF][JK_ESP_FS_PATHLEN];
static int jk_esp_fs_next = 0;

static const char* jk_esp_fs_base(void)
{
    if (jk_esp_fs_cwd[0]) return jk_esp_fs_cwd;
    return jk_esp_game_dir();
}

const char* jk_esp_fs_path(const char* path)
{
    if (!path) return path;
    if (path[0] == '/') return path;
    char* out = jk_esp_fs_bufs[jk_esp_fs_next];
    jk_esp_fs_next = (jk_esp_fs_next + 1) % JK_ESP_FS_NBUF;
    // strip a leading "./"
    while (path[0] == '.' && path[1] == '/') path += 2;
    if (path[0] == '.' && path[1] == 0) {
        snprintf(out, JK_ESP_FS_PATHLEN, "%s", jk_esp_fs_base());
    } else {
        snprintf(out, JK_ESP_FS_PATHLEN, "%s/%s", jk_esp_fs_base(), path);
    }
    // the engine sometimes builds paths with backslashes
    for (char* p = out; *p; p++) {
        if (*p == '\\') *p = '/';
    }
    return out;
}

char* jk_esp_getcwd(char* buf, size_t size)
{
    if (!buf || size == 0) return NULL;
    snprintf(buf, size, "%s", jk_esp_fs_base());
    return buf;
}

int jk_esp_chdir(const char* path)
{
    if (!path) return -1;
    if (path[0] == '/') {
        snprintf(jk_esp_fs_cwd, sizeof(jk_esp_fs_cwd), "%s", path);
    } else {
        snprintf(jk_esp_fs_cwd, sizeof(jk_esp_fs_cwd), "%s/%s", jk_esp_fs_base(), path);
    }
    // drop a trailing slash
    size_t n = strlen(jk_esp_fs_cwd);
    while (n > 1 && jk_esp_fs_cwd[n - 1] == '/') jk_esp_fs_cwd[--n] = 0;
    return 0;
}

// --- directory existence cache ---------------------------------------------
// Open-addressed hash set of directory paths with their existence, so the
// thousands of GOB-content lookups that probe the card first never repeat a
// FatFs directory scan (each of those costs a block read per directory
// sector).
#define JK_ESP_FS_DIRCACHE 512
#define JK_ESP_FS_DIRLEN 128
static char jk_esp_fs_dirs[JK_ESP_FS_DIRCACHE][JK_ESP_FS_DIRLEN];
static signed char jk_esp_fs_dir_state[JK_ESP_FS_DIRCACHE]; // 0 empty, 1 exists, -1 missing
static int jk_esp_fs_dir_count = 0;
uint32_t jk_esp_fs_dir_hits = 0, jk_esp_fs_dir_misses = 0, jk_esp_fs_dir_overflow = 0;

void jk_esp_fs_dir_cache_reset(void)
{
    memset(jk_esp_fs_dir_state, 0, sizeof(jk_esp_fs_dir_state));
    jk_esp_fs_dir_count = 0;
}

static uint32_t jk_esp_fs_hash(const char* s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

int jk_esp_fs_dir_might_exist(const char* abs_path)
{
    if (!abs_path) return 1;
    const char* slash = strrchr(abs_path, '/');
    if (!slash || slash == abs_path) return 1;
    size_t dlen = (size_t)(slash - abs_path);
    if (dlen >= JK_ESP_FS_DIRLEN) return 1;
    uint32_t h = jk_esp_fs_hash(abs_path, dlen);
    for (int probe = 0; probe < JK_ESP_FS_DIRCACHE; probe++) {
        uint32_t idx = (h + probe) % JK_ESP_FS_DIRCACHE;
        if (jk_esp_fs_dir_state[idx] == 0) {
            // not cached: ask the card once
            char dir[JK_ESP_FS_DIRLEN];
            memcpy(dir, abs_path, dlen);
            dir[dlen] = 0;
            struct stat st;
            int exists = (stat(dir, &st) == 0) && S_ISDIR(st.st_mode);
            jk_esp_fs_dir_misses++;
            if (jk_esp_fs_dir_count < JK_ESP_FS_DIRCACHE - 1) {
                strcpy(jk_esp_fs_dirs[idx], dir);
                jk_esp_fs_dir_state[idx] = exists ? 1 : -1;
                jk_esp_fs_dir_count++;
            } else {
                jk_esp_fs_dir_overflow++;
            }
            return exists;
        }
        if (strlen(jk_esp_fs_dirs[idx]) == dlen && !strncmp(jk_esp_fs_dirs[idx], abs_path, dlen)) {
            jk_esp_fs_dir_hits++;
            return jk_esp_fs_dir_state[idx] > 0;
        }
    }
    return 1;
}
