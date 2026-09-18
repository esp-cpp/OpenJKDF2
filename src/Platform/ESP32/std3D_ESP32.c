// std3D backend for the ESP32-P4 port: there is no 3D hardware, the world is
// drawn by the software rasterizer (RDRASTER_SOFTWARE_RENDERER) into an 8-bit
// tVBuffer, and std3D_DrawMenu hands the finished 8-bit frame (world + HUD
// composite, or the 2D menu) to the platform glue for palette conversion,
// scaling and display.
#include "Platform/std3D.h"

#include "General/stdMath.h"
#include "General/stdBitmap.h"
#include "Win95/stdDisplay.h"
#include "Win95/Video.h"
#include "Win95/Window.h"
#include "Main/jkGame.h"
#include "Main/Main.h"
#include "World/jkPlayer.h"
#include "Raster/rdZRaster.h"
#include "Engine/rdroid.h"
#include "Engine/rdMaterial.h"
#include "stdPlatform.h"
#include "jk.h"

#include "jk_esp.h"

int std3D_bReinitHudElements = 0;
int std3D_bHasInitted = 0;
size_t std3D_loadedTexturesAmt = 0;
rdDDrawSurface* std3D_aLoadedSurfaces[STD3D_MAX_TEXTURES] = {0};

extern rdColor24 stdDisplay_masterPalette[256];

int std3D_Startup()
{
    if (std3D_bHasInitted) return 1;
    memset(std3D_aLoadedSurfaces, 0, sizeof(std3D_aLoadedSurfaces));
    std3D_loadedTexturesAmt = 0;
    std3D_bHasInitted = 1;
    return 1;
}

void std3D_Shutdown()
{
    std3D_bHasInitted = 0;
}

int std3D_StartScene()
{
    // the material LRU / deferred loader budgets per frame on this counter
    std3D_frameCount++;
    return 1;
}

int std3D_EndScene()
{
    return 1;
}

void std3D_ResetRenderList()
{
}

int std3D_RenderListVerticesFinish()
{
    return 1;
}

void std3D_DrawRenderList()
{
    // nothing: rdCache_Flush takes its software branch (rdroid_curAcceleration == 0)
}

int std3D_SetCurrentPalette(rdColor24 *a1, int a2)
{
    return 1;
}

void std3D_GetValidDimension(unsigned int inW, unsigned int inH, unsigned int *outW, unsigned int *outH)
{
    *outW = inW;
    *outH = inH;
}

int std3D_DrawOverlay()
{
    return 1;
}

void std3D_UnloadAllTextures()
{
    std3D_PurgeEntireTextureCache();
}

#ifndef RDCACHE_RENDER_NGONS
void std3D_AddRenderListTris(rdTri *tris, unsigned int num_tris) {}
#else
void std3D_AddRenderListNGons(rdNGon *ngons, unsigned int num_ngons) {}
#endif
void std3D_AddRenderListLines(rdLine* lines, uint32_t num_lines) {}
int std3D_AddRenderListVertices(D3DVERTEX *vertex_array, int count) { return 1; }

void std3D_UpdateFrameCount(rdDDrawSurface *pTexture) {}
void std3D_RemoveTextureFromCacheList(rdDDrawSurface *pCacheTexture) {}
void std3D_AddTextureToCacheList(rdDDrawSurface *pTexture) {}

int std3D_PurgeTextureCache(size_t size)
{
    return 1;
}

void std3D_PurgeEntireTextureCache()
{
    for (int i = 0; i < STD3D_MAX_TEXTURES; i++) {
        if (std3D_aLoadedSurfaces[i]) {
            std3D_aLoadedSurfaces[i]->texture_loaded = 0;
            std3D_aLoadedSurfaces[i] = NULL;
        }
    }
    std3D_loadedTexturesAmt = 0;
}

int std3D_ClearZBuffer()
{
    return 1;
}

int std3D_AddToTextureCache(tVBuffer *vbuf, rdDDrawSurface *texture, int is_alpha_tex, int no_alpha)
{
    // The software rasterizer samples texels straight from the system-RAM
    // tVBuffer, so "loading" a texture is a no-op; mark it loaded so the
    // material code doesn't retry every frame.
    if (texture) {
        texture->texture_loaded = 1;
    }
    return 1;
}

// Present the frame. When the software renderer drew the world this frame the
// composite (world + HUD) is in Video_pSwWorldBuffer; otherwise (menus,
// cutscenes) the 2D menu buffer's 640x480 logical area is shown.
void std3D_DrawMenu()
{
    if (Main_bHeadless) return;

    tVBuffer* pSrc = NULL;
    int w = 0, h = 0;
    // the HUD (drawn into the 640x480 logical area of the menu buffer) is
    // handed to the platform as a separate full-resolution overlay instead of
    // being downsampled into the low-resolution world buffer
    const uint8_t* pOverlay = NULL;
    int ow = 0, oh = 0, opitch = 0;
#ifdef RDRASTER_SOFTWARE_RENDERER
    if (Video_swWorldPresentPending && Video_pSwWorldBuffer && Video_pSwWorldBuffer->surface_lock_alloc) {
        Video_swWorldPresentPending = 0;
        pSrc = Video_pSwWorldBuffer;
        w = pSrc->format.width;
        h = pSrc->format.height;
        if (Video_menuBuffer.surface_lock_alloc) {
            pOverlay = (const uint8_t*)Video_menuBuffer.surface_lock_alloc;
            ow = Video_menuBuffer.format.width < 640 ? Video_menuBuffer.format.width : 640;
            oh = Video_menuBuffer.format.height < 480 ? Video_menuBuffer.format.height : 480;
            opitch = Video_menuBuffer.format.rowSize;
        }
    }
#endif
    if (!pSrc) {
        if (!Video_menuBuffer.surface_lock_alloc) return;
        pSrc = &Video_menuBuffer;
        w = pSrc->format.width < 640 ? pSrc->format.width : 640;
        h = pSrc->format.height < 480 ? pSrc->format.height : 480;
    }
    if (w <= 0 || h <= 0) return;
#if defined(JK_ESP_FS_DEBUG)
    {
        static uint32_t n = 0;
        if ((n++ % 100) == 0) {
            const uint8_t* px = (const uint8_t*)pSrc->surface_lock_alloc;
            uint32_t nonzero = 0;
            for (int i = 0; i < w * h; i += 97) nonzero += px[i] != 0;
            extern int rdroid_curAcceleration;
            jk_esp_log("draw: sw=%d accel=%d isDDraw=%d worldbuf=%p src=%s %dx%d nonzero~%lu/%d",
                       rdroid_bSoftwareRenderer, rdroid_curAcceleration, jkGame_isDDraw, (void*)Video_pSwWorldBuffer,
                       pSrc == Video_pSwWorldBuffer ? "world" : "menu", w, h, (unsigned long)nonzero, (w * h) / 97);
        }
    }
#endif
    jk_esp_present_8bpp_overlay((const uint8_t*)pSrc->surface_lock_alloc, w, h, pSrc->format.rowSize,
                                pOverlay, ow, oh, opitch, (const uint8_t*)stdDisplay_masterPalette);
}

void std3D_DrawSceneFbo() {}
void std3D_FreeResources() {}
void std3D_InitializeViewport(rdRect *viewRect) {}
int std3D_GetValidDimensions(int a1, int a2, int a3, int a4) { return 1; }
int std3D_FindClosestDevice(uint32_t index, int a2) { return 1; }
int std3D_SetRenderList(intptr_t a1) { return 1; }
intptr_t std3D_GetRenderList() { return 0; }
int std3D_CreateExecuteBuffer() { return 1; }
int std3D_HasAlpha() { return 0; }
int std3D_HasAlphaFlatStippled() { return 0; }
int std3D_HasModulateAlpha() { return 0; }

void std3D_PurgeBitmapRefs(stdBitmap *pBitmap) {}
void std3D_PurgeSurfaceRefs(rdDDrawSurface *texture)
{
    for (int i = 0; i < STD3D_MAX_TEXTURES; i++) {
        if (std3D_aLoadedSurfaces[i] == texture) {
            std3D_aLoadedSurfaces[i] = NULL;
        }
    }
    if (texture) texture->texture_loaded = 0;
}
void std3D_PurgeTextureEntry(int i) {}
void std3D_PurgeUIEntry(int i, int idx) {}
void std3D_UpdateSettings() {}
void std3D_Screenshot(const char* pFpath) {}
#ifdef RDRASTER_SOFTWARE_RENDERER
void std3D_ScreenshotWindow(const char* pFpath) {}
#endif
void std3D_ResetUIRenderList() {}
int std3D_AddBitmapToTextureCache(stdBitmap *texture, int mipIdx, int is_alpha_tex, int no_alpha) { return 1; }
void std3D_DrawUIBitmapRGBA(stdBitmap* pBmp, int mipIdx, flex_t dstX, flex_t dstY, rdRect* srcRect, flex_t scaleX, flex_t scaleY, int bAlphaOverwrite, uint8_t color_r, uint8_t color_g, uint8_t color_b, uint8_t color_a) {}
void std3D_DrawUIBitmap(stdBitmap* pBmp, int mipIdx, flex_t dstX, flex_t dstY, rdRect* srcRect, flex_t scale, int bAlphaOverwrite) {}
void std3D_DrawUIClearedRect(uint8_t palIdx, rdRect* dstRect) {}
void std3D_DrawUIClearedRectRGBA(uint8_t color_r, uint8_t color_g, uint8_t color_b, uint8_t color_a, rdRect* dstRect) {}
int std3D_IsReady() { return std3D_bHasInitted; }
