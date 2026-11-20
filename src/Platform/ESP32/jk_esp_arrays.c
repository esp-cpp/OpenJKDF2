// The engine's largest arrays, allocated for one engine session.
//
// The firmware runs one game at a time, but every core's static .bss is
// reserved in PSRAM all the time, so the engine's biggest tables used to sit
// there unused while the other cores ran. With PLATFORM_SESSION_ARRAYS (see
// engine_config.h) they are pointers instead: tab5-emu's glue (jk.cpp) calls
// jk_esp_arrays_alloc() when Jedi Knight is launched, before the engine task
// starts, and jk_esp_arrays_free() once the engine (and the audio task) has
// stopped. Each launch gets fresh zeroed buffers, as static storage is at
// boot.
//
// They are one block from the engine heap (jk_esp_malloc: the ROM arena first,
// then PSRAM), taken before anything else, so it sits at the start of the
// fresh arena and cannot fragment the heap the level is loaded into.
#include <stdint.h>
#include <string.h>

#include "types.h"
#include "globals.h"
#include "AI/sithAI.h"
#include "Engine/rdActive.h"
#include "World/jkPlayer.h"

#include "jk_esp.h"

#ifndef PLATFORM_SESSION_ARRAYS
#error "jk_esp_arrays.c needs PLATFORM_SESSION_ARRAYS (engine_config.h)"
#endif

// X(pointer, element type, count). The counts are the static arrays' sizes
// (symbols.syms for the generated globals); the AI actors, the rasterizer
// pools and the directory cache take theirs from the build (tab5-emu's
// components/jk/Kconfig, "Engine table sizes").
#ifdef JKM_DSS
#define JK_ESP_MOTS_ARRAYS(X) X(jkPlayer_aMotsInfos, jkPlayerInfo, NUM_JKPLAYER_THINGS)
#else
#define JK_ESP_MOTS_ARRAYS(X)
#endif

// The active-edge rasterizer's pools (616 KB at JK.EXE's sizes) are only
// needed when it draws the world; with RDRASTER_SW_ZBUFFER rdZRaster does,
// and rdActive_DrawScene() never runs, so they are not allocated at all.
#if defined(RDRASTER_SOFTWARE_RENDERER) && !defined(RDRASTER_SW_ZBUFFER)
#define JK_ESP_RDACTIVE_ARRAYS(X) \
    X(aActiveFaces, rdActiveFace, RDACTIVE_MAX_FACES) \
    X(aActiveSpans, rdActiveSpan, RDACTIVE_MAX_SPANS)
#else
#define JK_ESP_RDACTIVE_ARRAYS(X)
#endif

#define JK_ESP_SESSION_ARRAYS(X) \
    X(sithAI_actors, SithAIControlBlock, SITHAI_MAX_ACTORS) \
    X(jkPlayer_playerInfos, SithPlayer, JKPLAYER_NUM_INFOS) \
    X(jkPlayer_otherThings, jkPlayerInfo, NUM_JKPLAYER_THINGS) \
    JK_ESP_MOTS_ARRAYS(X) \
    X(aEntryStack, char, 0x14 * (STDCONF_LINEBUFFER_LEN + 4)) \
    X(sithCollision_aCollisions, sithCollisionSearchResult, 4) \
    X(sithSurface_aSurfaces, rdSurface, 256) \
    JK_ESP_RDACTIVE_ARRAYS(X)

#define JK_ESP_ARRAY_ALIGN(n) (((size_t)(n) + 15) & ~(size_t)15)

static uint8_t* jk_esp_arrays_block = NULL;
static size_t jk_esp_arrays_size = 0;

// Point every array into `base` (NULL: detach them all); returns the bytes
// the layout needs.
static size_t jk_esp_arrays_layout(uint8_t* base)
{
    size_t off = 0;
#define JK_ESP_ARRAY_PLACE(var, type, count) \
    var = base ? (type*)(base + off) : NULL; \
    off += JK_ESP_ARRAY_ALIGN(sizeof(type) * (size_t)(count));
    JK_ESP_SESSION_ARRAYS(JK_ESP_ARRAY_PLACE)
#undef JK_ESP_ARRAY_PLACE
    // the port's directory-existence cache (jk_esp_fs.c)
    jk_esp_fs_dir_cache_attach(base ? base + off : NULL);
    off += JK_ESP_ARRAY_ALIGN(jk_esp_fs_dir_cache_bytes());
    return off;
}

size_t jk_esp_arrays_bytes(void)
{
    return jk_esp_arrays_size;
}

int jk_esp_arrays_alloc(void)
{
    jk_esp_arrays_free(); // a block left from an earlier launch (not expected)
    const size_t bytes = jk_esp_arrays_layout(NULL);
    uint8_t* block = (uint8_t*)jk_esp_malloc(bytes); // zeroed
    if (!block) {
        jk_esp_log("session arrays: no memory for %u KB", (unsigned)(bytes / 1024));
        return 0;
    }
    jk_esp_arrays_block = block;
    jk_esp_arrays_size = bytes;
    jk_esp_arrays_layout(block);
    jk_esp_log("session arrays: %u KB at %p (AI actors %d, dir cache %d)", (unsigned)(bytes / 1024), (void*)block,
               (int)SITHAI_MAX_ACTORS, (int)JK_ESP_FS_DIRCACHE);
    return 1;
}

void jk_esp_arrays_free(void)
{
    if (!jk_esp_arrays_block) {
        return;
    }
    jk_esp_arrays_layout(NULL);
    // the one global pointer into these arrays (sithClose resets it when a
    // level closes; cleared here too, so nothing can follow it into the
    // freed block)
    sithPlayer_g_pLocalPlayer = NULL;
    jk_esp_free(jk_esp_arrays_block);
    jk_esp_arrays_block = NULL;
    jk_esp_arrays_size = 0;
}
