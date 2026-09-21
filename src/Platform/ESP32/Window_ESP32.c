// Window / main-loop layer for the ESP32-P4 port.
//
// The engine's original structure is a Win32 message loop; the retro ports
// (see Platform/TWL/Window_Twl.c) drive it from a plain loop and synthesize
// the messages. Here the loop is split into startup / one frame / shutdown so
// the tab5-emu cart can run it from its own task and interleave the pause
// menu.
#include "Win95/Window.h"
#include "Win95/stdGdi.h"
#include "Platform/std3D.h"
#include "Main/Main.h"
#include "Main/jkMain.h"
#include "Main/jkGame.h"
#include "Gui/jkGUI.h"
#include "Win95/stdDisplay.h"
#include "World/jkPlayer.h"
#include "Platform/stdControl.h"
#include "stdPlatform.h"
#include "Devices/sithConsole.h"
#include "Platform/wuRegistry.h"
#include "Main/jkQuakeConsole.h"
#include "Gui/jkGUIRend.h"
#include "Win95/Video.h"
#include "World/sithWorld.h"

#include "jk_esp.h"

#include <stdio.h>
#include <string.h>

extern int jkGuiBuildMulti_bRendering;

int Window_lastXRel = 0;
int Window_lastYRel = 0;
int Window_lastSampleTime = 0;
int Window_lastSampleMs = 0;
int Window_bMouseLeft = 0;
int Window_bMouseRight = 0;
int Window_resized = 0;
int Window_mouseX = 0;
int Window_mouseY = 0;
int Window_mouseWheelX = 0;
int Window_mouseWheelY = 0;
int Window_lastMouseX = 0;
int Window_lastMouseY = 0;
int Window_xPos = 0;
int Window_yPos = 0;
int last_jkGame_isDDraw = 0;
#ifdef QUAKE_CONSOLE
int last_jkQuakeConsole_bOpen = 0;
#endif
int Window_menu_mouseX = 0;
int Window_menu_mouseY = 0;
extern int Window_needsRecreate;
int Window_bFlipRequested = 0;

static int Window_bStarted = 0;
static int jkPlayer_enableVsync_last = 0;
static jk_esp_input_t Window_lastInput;

// Map the logical 1280x720 touch position onto the 640x480 (4:3, centered)
// menu surface the GUI is laid out for.
static void Window_TouchToMenu(int tx, int ty, int* mx, int* my)
{
    const int screen_w = 1280, screen_h = 720;
    const int menu_h = screen_h;
    const int menu_w = screen_h * 640 / 480;
    const int off_x = (screen_w - menu_w) / 2;
    int x = (tx - off_x) * 640 / menu_w;
    int y = ty * 480 / menu_h;
    if (x < 0) x = 0;
    if (x > 639) x = 639;
    if (y < 0) y = 0;
    if (y > 479) y = 479;
    *mx = x;
    *my = y;
}

void Window_Main_Loop()
{
    jkMain_GuiAdvance();
    Window_msg_main_handler(g_hWnd, WM_PAINT, 0, 0);
}

// --- entry points used by tab5-emu (components/jk/src/jk.cpp) -------------

int jk_esp_engine_startup(const char* game_dir)
{
    char cmdLine[256];
    strncpy(cmdLine, jk_esp_engine_args(), sizeof(cmdLine) - 1);
    cmdLine[sizeof(cmdLine) - 1] = 0;

    g_handler_count = 0;
    g_thing_two_some_dialog_count = 0;
    g_should_exit = 0;
    g_window_not_destroyed = 0;
    g_hInstance = 0;
    g_nShowCmd = 0;

    // the engine opens everything relative to the working directory
    if (game_dir && game_dir[0]) {
        chdir(game_dir);
    }

    Window_xSize = 640;
    Window_ySize = 480;
    Window_resized = 1;
    // platform state from a previous run in this process
    Window_bMouseLeft = Window_bMouseRight = 0;
    Window_mouseX = Window_mouseY = Window_lastMouseX = Window_lastMouseY = 0;
    Window_lastXRel = Window_lastYRel = 0;
    Window_menu_mouseX = Window_menu_mouseY = 0;
    Window_bFlipRequested = 0;
    last_jkGame_isDDraw = 0;
    memset(&Window_lastInput, 0, sizeof(Window_lastInput));

    int result = Main_Startup(cmdLine);
    if (!result) {
        jk_esp_log("Main_Startup failed");
        return 0;
    }

    std3D_FreeResources();

    g_window_not_destroyed = 1;
    Window_msg_main_handler(g_hWnd, WM_CREATE, 0, 0);
    Window_msg_main_handler(g_hWnd, WM_ACTIVATE, 2, 0);
    Window_msg_main_handler(g_hWnd, WM_ACTIVATEAPP, 1, 0);
    Window_msg_main_handler(g_hWnd, WM_SHOWWINDOW, 0, 0);
    Window_msg_main_handler(g_hWnd, WM_PAINT, 0, 0);
    Window_bStarted = 1;
    jk_esp_log("engine started");
    return 1;
}

int jk_esp_engine_frame(void)
{
    if (!Window_bStarted) return 0;
    static uint32_t frames = 0;
    static uint32_t lastReport = 0;
    frames++;
#if defined(JK_ESP_FS_DEBUG)
    {
        extern void jk_prof_start(void); extern void jk_prof_dump(int);
        static int profState = 0;
        if (profState == 0) { jk_prof_start(); profState = 1; }
        else if (profState == 1 && sithWorld_g_pLastLoadedWorld) { jk_prof_dump(48); profState = 2; }
    }
#endif
    // keep PSRAM headroom: the material cache only evicts on allocation
    // failure, which would otherwise come after the general heap has been
    // squeezed (fragmentation, allocations elsewhere failing first)
    if (jk_esp_psram_free() < 3u * 1024 * 1024) {
        extern int rdMaterial_PurgeMaterialCache(void);
        static uint32_t purges = 0;
        if (rdMaterial_PurgeMaterialCache()) {
            if ((++purges % 50) == 1) {
                jk_esp_log("material cache: purging (psram free %u KB)", (unsigned)(jk_esp_psram_free() / 1024));
            }
        }
    }
    uint32_t now = jk_esp_time_ms();
    if (now - lastReport >= 5000) {
        jk_esp_log("frame %lu: isDDraw=%d menu=%p world=%p", (unsigned long)frames, jkGame_isDDraw,
                   (void*)Video_menuBuffer.surface_lock_alloc, (void*)sithWorld_g_pLastLoadedWorld);
        jk_esp_present_report();
        {
            extern void stdSound_ESP32_Report(void);
            stdSound_ESP32_Report();
        }
        lastReport = now;
    }
    Window_Main_Loop();
    if (g_should_exit || jk_esp_quit_requested) {
        jk_esp_log("engine frame: exit (g_should_exit=%d quit_requested=%d)", g_should_exit, jk_esp_quit_requested);
        return 0;
    }
    return 1;
}

void jk_esp_engine_shutdown(void)
{
    if (!Window_bStarted) return;
    if (jkPlayer_bHasLoadedSettingsOnce) {
        jkPlayer_WriteConf(jkPlayer_playerShortName);
    }
    // The platform quits from anywhere (usually mid-level), which skips the
    // GUI state machine's gameplay teardown: close the level like leaving
    // gameplay for the main menu does (frees the world, clears
    // jkGame_isDDraw and the cameras' focus things), otherwise the next
    // launch in this process inherits the stale state.
    if (jkMain_bInit || jkGame_isDDraw) {
        jk_esp_log("shutdown: leaving gameplay (bInit=%d isDDraw=%d)", jkMain_bInit, jkGame_isDDraw);
        jkMain_GameplayLeave(JK_GAMEMODE_GAMEPLAY, JK_GAMEMODE_MAIN);
    }
    Main_Shutdown();
    Window_bStarted = 0;
    {
        extern void stdDisplay_ESP32_FreeBuffers(void);
        extern void jk_esp_file_dump_open(void);
        stdDisplay_ESP32_FreeBuffers();
        jk_esp_file_dump_open();
    jk_esp_file_release_buffers();
    }
}

int Window_Main_Linux(int argc, char** argv)
{
    if (!jk_esp_engine_startup(NULL)) return 0;
    while (jk_esp_engine_frame()) {}
    jk_esp_engine_shutdown();
    return 1;
}

int Window_DefaultHandler(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam, void* unused)
{
    return 0;
}

int Window_ShowCursorUnwindowed(int a1)
{
    return stdControl_ShowMouseCursor(a1);
}

void jkGuiRend_ESP32_CancelActiveMenu(void);

int Window_MessageLoop()
{
    // the engine's menus are modal loops inside one "frame": pause / resume
    // (the emulator's own menu) and quit have to work from here too
    if (jk_esp_park_point()) {
        Window_bFlipRequested = 1; // redraw after the pause
    }
    if (jk_esp_quit_requested) {
        jkGuiRend_ESP32_CancelActiveMenu();
        return 0;
    }
    jkGuiRend_UpdateController();
    jk_esp_input_t in;
    jk_esp_read_input(&in);
    const int keysChanged = memcmp(in.keys, Window_lastInput.keys, sizeof(in.keys)) != 0;
    const int mouseActive = in.mouse_dx || in.mouse_dy || in.mouse_wheel
                         || in.mouse_buttons != Window_lastInput.mouse_buttons;
    if (in.touch_down || Window_lastInput.touch_down || Window_bFlipRequested || keysChanged || mouseActive) {
        Window_msg_main_handler(g_hWnd, WM_PAINT, 0, 0);
        Window_SdlUpdate();
        Window_bFlipRequested = 0;
    }
    return 0;
}

// HID keyboard usage -> the Windows virtual key the GUI reacts to (0 = none)
// and the character for WM_CHAR (0 = none), for menu navigation / text entry.
static int Window_HidUsageToVk(int usage, int shift, int* pChr)
{
    *pChr = 0;
    if (usage >= 4 && usage <= 29) {            // a..z
        *pChr = (shift ? 'A' : 'a') + (usage - 4);
        return 'A' + (usage - 4);
    }
    if (usage >= 30 && usage <= 39) {           // 1..9, 0
        static const char digits[] = "1234567890";
        static const char shifted[] = "!@#$%^&*()";
        *pChr = shift ? shifted[usage - 30] : digits[usage - 30];
        return '0' + ((usage - 30 + 1) % 10);
    }
    switch (usage) {
    case 40: *pChr = '\r'; return VK_RETURN;
    case 41: *pChr = 27; return VK_ESCAPE;
    case 42: *pChr = 8; return VK_BACK;
    case 43: *pChr = '\t'; return VK_TAB;
    case 44: *pChr = ' '; return VK_SPACE;
    case 45: *pChr = shift ? '_' : '-'; return 0xBD;
    case 46: *pChr = shift ? '+' : '='; return 0xBB;
    case 54: *pChr = shift ? '<' : ','; return 0xBC;
    case 55: *pChr = shift ? '>' : '.'; return 0xBE;
    case 76: return VK_DELETE;
    case 79: return VK_RIGHT;
    case 80: return VK_LEFT;
    case 81: return VK_DOWN;
    case 82: return VK_UP;
    case 88: *pChr = '\r'; return VK_RETURN;   // keypad enter
    default: break;
    }
    if (usage >= 58 && usage <= 69) {           // F1..F12
        return 0x70 + (usage - 58);
    }
    return 0;
}

// Pump input into the engine's window message handler and handle resizes.
void Window_SdlUpdate()
{
    if (Main_bHeadless) {
        return;
    }

    // Dispatching a message below can redraw the GUI, which flips, which
    // calls back into here: commit the new input state first and refuse to
    // nest, so an edge is reported exactly once.
    static int Window_bInUpdate = 0;
    if (Window_bInUpdate) {
        return;
    }
    Window_bInUpdate = 1;
    jk_esp_input_t in;
    jk_esp_read_input(&in);
    const jk_esp_input_t last = Window_lastInput;
    Window_lastInput = in;
    const uint16_t pressed = in.buttons & ~last.buttons;
    const uint16_t released = ~in.buttons & last.buttons;

    // START -> escape (pause menu / back), A -> return, d-pad -> arrows
    struct { uint16_t mask; int vk; int chr; } keymap[] = {
        { 1 << 5, VK_ESCAPE, 1 }, // start
        { 1 << 0, VK_RETURN, 1 }, // a
        { 1 << 6, VK_UP, 0 },
        { 1 << 7, VK_DOWN, 0 },
        { 1 << 8, VK_LEFT, 0 },
        { 1 << 9, VK_RIGHT, 0 },
    };
    for (size_t i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
        if (pressed & keymap[i].mask) {
            Window_msg_main_handler(g_hWnd, WM_KEYFIRST, keymap[i].vk, 0);
            if (keymap[i].chr)
                Window_msg_main_handler(g_hWnd, WM_CHAR, keymap[i].vk, 0);
        }
        if (released & keymap[i].mask) {
            Window_msg_main_handler(g_hWnd, WM_KEYUP, keymap[i].vk, 0);
        }
    }

    // USB keyboard -> key messages for the 2D GUI (menus, text entry)
    {
        const int shift = ((in.keys[0xE1 >> 3] >> (0xE1 & 7)) & 1) || ((in.keys[0xE5 >> 3] >> (0xE5 & 7)) & 1);
        for (int usage = 4; usage < 0xE0; usage++) {
            const int now = (in.keys[usage >> 3] >> (usage & 7)) & 1;
            const int was = (last.keys[usage >> 3] >> (usage & 7)) & 1;
            if (now == was) continue;
            int chr = 0;
            const int vk = Window_HidUsageToVk(usage, shift, &chr);
            if (!vk) continue;
            if (now) {
                Window_msg_main_handler(g_hWnd, WM_KEYFIRST, vk, 0);
                if (chr)
                    Window_msg_main_handler(g_hWnd, WM_CHAR, chr, 0);
            } else {
                Window_msg_main_handler(g_hWnd, WM_KEYUP, vk, 0);
            }
        }
    }

    // USB mouse -> cursor for the 2D GUI (in-game, stdControl takes the
    // motion as look input instead)
    if (!jkGame_isDDraw && (in.mouse_dx || in.mouse_dy || in.mouse_buttons != last.mouse_buttons)) {
        int dx = 0, dy = 0, wheel = 0;
        jk_esp_mouse_take(&dx, &dy, &wheel);
        Window_mouseX += dx;
        Window_mouseY += dy;
        if (Window_mouseX < 0) Window_mouseX = 0;
        if (Window_mouseY < 0) Window_mouseY = 0;
        if (Window_mouseX > 639) Window_mouseX = 639;
        if (Window_mouseY > 479) Window_mouseY = 479;
        uint32_t pos = (Window_mouseX & 0xFFFF) | ((Window_mouseY << 16) & 0xFFFF0000);
        Window_msg_main_handler(g_hWnd, WM_MOUSEMOVE, 0, pos);
        const int leftNow = in.mouse_buttons & 1, leftWas = last.mouse_buttons & 1;
        if (leftNow && !leftWas) {
            Window_bMouseLeft = 1;
            Window_msg_main_handler(g_hWnd, WM_LBUTTONDOWN, 1, pos);
        } else if (!leftNow && leftWas) {
            Window_bMouseLeft = 0;
            Window_msg_main_handler(g_hWnd, WM_LBUTTONUP, 0, pos);
        }
    }

    // Touch -> mouse for the 2D GUI (menus). In-game, stdControl reads the
    // touch as relative look input instead.
    if (!jkGame_isDDraw) {
        if (in.touch_down) {
            int mx, my;
            Window_TouchToMenu(in.touch_x, in.touch_y, &mx, &my);
            Window_mouseX = mx;
            Window_mouseY = my;
            uint32_t pos = (Window_mouseX & 0xFFFF) | ((Window_mouseY << 16) & 0xFFFF0000);
            Window_msg_main_handler(g_hWnd, WM_MOUSEMOVE, 0, pos);
            if (!last.touch_down) {
                Window_bMouseLeft = 1;
                Window_msg_main_handler(g_hWnd, WM_LBUTTONDOWN, 1, pos);
            }
        } else if (last.touch_down) {
            uint32_t pos = (Window_mouseX & 0xFFFF) | ((Window_mouseY << 16) & 0xFFFF0000);
            Window_bMouseLeft = 0;
            Window_msg_main_handler(g_hWnd, WM_LBUTTONUP, 0, pos);
        }
    }

    // Present the 2D menu buffer (in-game frames are presented from
    // jkGame_Update via std3D_DrawMenu)
    if (!jkGame_isDDraw && !jkGuiBuildMulti_bRendering) {
        std3D_StartScene();
        std3D_DrawMenu();
        std3D_EndScene();
    }

    if (Window_resized)
    {
        jkMain_FixRes();
        if (!jkGui_SetModeMenu(0))
        {
            // fallthrough
        }
        Window_resized = 0;
        if (jkGame_isDDraw != last_jkGame_isDDraw) {
            Window_menu_mouseX = Window_mouseX;
            Window_menu_mouseY = Window_mouseY;
            Window_lastXRel = 0;
            Window_lastYRel = 0;
        }
    }
    jkPlayer_enableVsync_last = jkPlayer_enableVsync;
    last_jkGame_isDDraw = jkGame_isDDraw;
#ifdef QUAKE_CONSOLE
    last_jkQuakeConsole_bOpen = jkQuakeConsole_bOpen;
#endif
    Window_bInUpdate = 0;
}

void Window_SdlUpdateModal()
{
    Window_SdlUpdate();
}

void Window_SdlVblank()
{
    if (Main_bHeadless) return;
}

void Window_RecreateSDL2Window() {}
