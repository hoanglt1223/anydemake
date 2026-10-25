#pragma once
#include <stddef.h>
#include <wchar.h>

#include "../proxy/demake-config.h"

enum { DEMAKE_API_D3D11 = 1, DEMAKE_API_D3D12 = 2, DEMAKE_API_ALL = 3 };

typedef struct demake_preset {
    const char *name;
    const wchar_t *label;
    demake_config cfg;
} demake_preset;

typedef struct demake_options {
    demake_config cfg;
    unsigned apis;  /* DEMAKE_API_* bits */
    int unreal;     /* write Engine.ini settings when the folder is an Unreal game */
    int force;      /* back up and replace a d3d11.dll/d3d12.dll from another mod */
} demake_options;

extern const demake_preset demake_presets[];
extern const int demake_preset_count;

void demake_options_default(demake_options *o);
const demake_preset *demake_find_preset(const char *name);

/* dir is the folder with the game's .exe, or the root of an Unreal game.
   Returns 0 or a negative error; msg gets a readable report either way. */
int demake_install(const wchar_t *dir, const demake_options *o, wchar_t *msg, size_t msg_len);
/* -2: a proxy DLL from another mod is in the way and force was not set. */
#define DEMAKE_ERR_FOREIGN (-2)

/* Removes what AnyDemake wrote (DLLs and Engine.ini block) and restores backups. */
int demake_remove(const wchar_t *dir, wchar_t *msg, size_t msg_len);

/* Reads the baked config of a DLL file or of the DLLs installed in dir.
   apis (optional) receives which proxies are installed. */
int demake_read(const wchar_t *path, demake_config *cfg, unsigned *apis);

/* ---- Unreal Engine (unreal.c) ---- */

/* Finds the project of an Unreal game from its root or Binaries\Win64 folder.
   bin_dir receives the folder with the real game .exe. */
int unreal_find(const wchar_t *dir, wchar_t *project, wchar_t *bin_dir);
int unreal_write(const wchar_t *project, const demake_config *cfg, wchar_t *msg, size_t msg_len);
int unreal_remove(const wchar_t *project, wchar_t *msg, size_t msg_len);
