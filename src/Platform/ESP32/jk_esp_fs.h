// ESP-IDF's VFS has no working directory: relative paths never resolve.
// The engine (and the retro ports it inherits from) opens everything relative
// to the game directory, so this header - force-included into every engine
// translation unit on ESP32 - rewrites the libc file-system entry points to
// prefix relative paths with the game directory, and gives getcwd()/chdir()
// the same idea of "current directory".
//
// The system headers are included first so that their declarations are not
// mangled by the function-like macros below; later includes are no-ops
// thanks to their include guards.
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

// Absolute form of `path` (unchanged if it already is absolute). The result
// lives in a small ring of static buffers, valid until a few more calls.
const char* jk_esp_fs_path(const char* path);
char* jk_esp_getcwd(char* buf, size_t size);
int jk_esp_chdir(const char* path);
// 1 if the directory containing `abs_path` is known to exist (or unknown),
// 0 if it is known not to exist. Learns from stat() results; call
// jk_esp_fs_dir_cache_reset() after creating directories.
int jk_esp_fs_dir_might_exist(const char* abs_path);
void jk_esp_fs_dir_cache_reset(void);

#ifdef __cplusplus
}
#endif

#define fopen(p, m)     fopen(jk_esp_fs_path(p), m)
#define stat(p, s)      stat(jk_esp_fs_path(p), s)
#define lstat(p, s)     stat(jk_esp_fs_path(p), s)
#define opendir(p)      opendir(jk_esp_fs_path(p))
#define scandir(p, ...) scandir(jk_esp_fs_path(p), __VA_ARGS__)
#define mkdir(p, m)     mkdir(jk_esp_fs_path(p), m)
#define rmdir(p)        rmdir(jk_esp_fs_path(p))
#define unlink(p)       unlink(jk_esp_fs_path(p))
#define remove(p)       remove(jk_esp_fs_path(p))
#define getcwd(b, n)    jk_esp_getcwd(b, n)
#define chdir(p)        jk_esp_chdir(p)
