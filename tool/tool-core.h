#pragma once
#include <stddef.h>
#include <wchar.h>

#include "../proxy/demake-config.h"

typedef struct demake_preset {
    const char *name;
    const wchar_t *label;
    demake_config cfg;
} demake_preset;

extern const demake_preset demake_presets[];
extern const int demake_preset_count;

void demake_config_default(demake_config *cfg);
const demake_preset *demake_find_preset(const char *name);

/* Writes <dir>\d3d11.dll from the embedded template with cfg baked in.
   A d3d11.dll that AnyDemake did not write is backed up first, only when
   force is set. Returns 0 or a negative error; msg gets a readable reason. */
int demake_install(const wchar_t *dir, const demake_config *cfg, int force, wchar_t *msg, size_t msg_len);

/* Removes <dir>\d3d11.dll if AnyDemake wrote it and restores a backup. */
int demake_remove(const wchar_t *dir, wchar_t *msg, size_t msg_len);

/* Reads the baked config of a DLL file (or <dir>\d3d11.dll). */
int demake_read(const wchar_t *path, demake_config *cfg);
