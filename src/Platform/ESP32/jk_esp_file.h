#pragma once
// Buffered file I/O for the engine's HostServices on ESP32. Reads go through
// a small set of cache windows per file, so the GOB layer's constant seeking
// between entries of the same archive does not turn every line read into an
// SD card block transfer (the C library's stdio drops its buffer on seek).
#include <stddef.h>
#include <stdint.h>
#include <uchar.h>

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

stdFile_t jk_esp_file_open(const char* fpath, const char* mode);
/// Log the files still open (diagnostics, e.g. after engine shutdown)
void jk_esp_file_dump_open(void);
int jk_esp_file_close(stdFile_t f);
size_t jk_esp_file_read(stdFile_t f, void* dst, size_t len);
size_t jk_esp_file_write(stdFile_t f, void* src, size_t len);
const char* jk_esp_file_gets(stdFile_t f, char* dst, size_t len);
const char16_t* jk_esp_file_getws(stdFile_t f, char16_t* dst, size_t len);
int jk_esp_file_eof(stdFile_t f);
int jk_esp_file_tell(stdFile_t f);
int jk_esp_file_seek(stdFile_t f, int offset, int whence);
int jk_esp_file_size(stdFile_t f);
int jk_esp_file_printf(stdFile_t f, const char* fmt, ...);

#ifdef __cplusplus
}
#endif
