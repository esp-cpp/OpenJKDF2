// Save / load / restart entry points for the tab5-emu pause menu. All of these
// must run on the engine thread (they touch engine state); the glue posts
// requests and the engine loop calls them between frames.
#include "Dss/sithGamesave.h"
#include "Main/Main.h"
#include "Main/jkMain.h"
#include "Main/jkSmack.h"
#include "Gui/jkGUITitle.h"
#include "World/jkPlayer.h"
#include "World/sithWorld.h"
#include "Gameplay/sithPlayer.h"
#include "Cog/jkCog.h"
#include "General/stdString.h"
#include "stdPlatform.h"
#include "jk.h"

#include "jk_esp.h"

#include <stdio.h>
#include <string.h>

// Save the running game to `path` (absolute; see the TARGET_ESP32 branch in
// sithGamesave_Save). The save name shown by the engine's own load menu is
// "<level>~<label>". Returns 1 on success.
int jk_esp_engine_save(const char* path, const char* label)
{
    if (!sithWorld_g_pCurrentWorld || !sithPlayer_g_pLocalPlayerThing) {
        jk_esp_log("save: no level running");
        return 0;
    }
    if (sithPlayer_g_pLocalPlayerThing->flags & SITH_TF_DEAD) {
        jk_esp_log("save: player is dead");
        return 0;
    }
    char16_t wLabel[64];
    stdString_CharToWchar(wLabel, label ? label : "tab5", 63);
    wLabel[63] = 0;
    char16_t* wLevel = jkGuiTitle_quicksave_related_func1(&jkCog_strings, sithWorld_g_pCurrentWorld->map_jkl_fname);
    char16_t saveName[256];
    jk_snwprintf(saveName, 256, u"%s~%s", wLevel ? wLevel : u"", wLabel);
    if (!sithGamesave_Save((char*)path, 1, 1, saveName)) {
        jk_esp_log("save: sithGamesave_Save refused");
        return 0;
    }
    sithGamesave_Process(); // writes the file now
    jk_esp_log("save: wrote %s", path);
    return 1;
}

// Load the save at `path`. Same level: restored in place on the next game
// tick. Different (or no) level: the engine switches to that level and
// restores, like the engine's own load menu. Returns 1 if the load was
// scheduled.
int jk_esp_engine_load(const char* path)
{
    sithGamesave_Header header;
    FILE* f = fopen(path, "rb");
    if (!f) {
        jk_esp_log("load: cannot open %s", path);
        return 0;
    }
    size_t n = fread(&header, 1, sizeof(header), f);
    fclose(f);
    if (n != sizeof(header) || (header.version != 6 && header.version != 0x7D6)) {
        jk_esp_log("load: %s is not a JK save (read %u, version %d)", path, (unsigned)n, (int)header.version);
        return 0;
    }
    if (sithWorld_g_pCurrentWorld
        && !__strcmpi(header.episodeName, sithWorld_g_pCurrentWorld->episodeName)
        && !__strcmpi(header.jklName, sithWorld_g_pCurrentWorld->map_jkl_fname)) {
        jk_esp_log("load: restoring %s in place", path);
        return jkPlayer_LoadSave((char*)path);
    }
    // the part of the save name after '~' is the user's label
    char16_t* label = header.saveName;
    for (char16_t* p = header.saveName; *p; p++) {
        if (*p == u'~') { label = p + 1; break; }
    }
    jk_esp_log("load: switching to episode '%s' level '%s' for %s", header.episodeName, header.jklName, path);
    jkSmack_gameMode = 1; // "load a save" flavour of the level switch
    return jkMain_sub_4034D0(header.episodeName, (char*)path, header.jklName, label);
}

// Restart the current level (the engine's "restart level" action).
int jk_esp_engine_reset(void)
{
    if (!sithWorld_g_pCurrentWorld) {
        jk_esp_log("reset: no level running");
        return 0;
    }
    return jkMain_MissionReload();
}
