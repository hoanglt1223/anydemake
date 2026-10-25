#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tool-core.h"
#include "resource.h"

#define CFG(mip, def, min, sampler, pix, bits) {DEMAKE_CONFIG_MAGIC, sizeof(demake_config), mip, def, min, sampler, pix, bits, 1}

const demake_preset demake_presets[] = {
    {"off", L"Off (passthrough)", CFG(0, 0, 64, DEMAKE_SAMPLER_KEEP, 0, 0)},
    {"light", L"Light: half-size textures", CFG(1, 0, 64, DEMAKE_SAMPLER_BILINEAR, 0, 0)},
    {"ps2", L"PS2 look", CFG(2, 0, 64, DEMAKE_SAMPLER_BILINEAR, 2, 0)},
    {"ps1", L"PS1 look", CFG(3, 0, 32, DEMAKE_SAMPLER_POINT, 3, 5)},
    {"potato", L"Potato: lowest", CFG(4, 1, 16, DEMAKE_SAMPLER_POINT, 4, 4)},
};
const int demake_preset_count = sizeof demake_presets / sizeof demake_presets[0];

void demake_config_default(demake_config *cfg)
{
    *cfg = demake_presets[0].cfg;
    cfg->log = 0;
}

const demake_preset *demake_find_preset(const char *name)
{
    for (int i = 0; i < demake_preset_count; i++)
        if (!strcmp(demake_presets[i].name, name))
            return &demake_presets[i];
    return NULL;
}

static unsigned char *read_file(const wchar_t *path, size_t *len)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    LARGE_INTEGER size;
    unsigned char *buf = NULL;
    DWORD got;
    if (GetFileSizeEx(f, &size) && size.QuadPart < 512 * 1024 * 1024 && (buf = malloc((size_t)size.QuadPart + 1)) &&
        ReadFile(f, buf, (DWORD)size.QuadPart, &got, NULL) && got == size.QuadPart) {
        *len = got;
    } else {
        free(buf);
        buf = NULL;
    }
    CloseHandle(f);
    return buf;
}

static demake_config *find_config(unsigned char *data, size_t len)
{
    size_t mlen = sizeof(DEMAKE_CONFIG_MAGIC) - 1;
    for (size_t i = 0; i + sizeof(demake_config) <= len; i++)
        if (!memcmp(data + i, DEMAKE_CONFIG_MAGIC, mlen))
            return (demake_config *)(data + i);
    return NULL;
}

static void dll_path(const wchar_t *dir, wchar_t *out)
{
    _snwprintf(out, MAX_PATH, L"%s\\d3d11.dll", dir);
    out[MAX_PATH - 1] = 0;
}

int demake_read(const wchar_t *path, demake_config *cfg)
{
    wchar_t buf[MAX_PATH];
    DWORD attr = GetFileAttributesW(path);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        dll_path(path, buf);
        path = buf;
    }
    size_t len;
    unsigned char *data = read_file(path, &len);
    if (!data)
        return -1;
    demake_config *found = find_config(data, len);
    if (found)
        memcpy(cfg, found, sizeof *cfg);
    free(data);
    return found ? 0 : -2;
}

int demake_install(const wchar_t *dir, const demake_config *cfg, int force, wchar_t *msg, size_t msg_len)
{
    HRSRC res = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_PROXY_DLL), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL h = res ? LoadResource(NULL, res) : NULL;
    const void *tmpl = h ? LockResource(h) : NULL;
    size_t len = res ? SizeofResource(NULL, res) : 0;
    if (!tmpl || !len) {
        _snwprintf(msg, msg_len, L"the template DLL is missing from this executable");
        return -1;
    }
    unsigned char *data = malloc(len);
    if (!data)
        return -1;
    memcpy(data, tmpl, len);
    demake_config *slot = find_config(data, len);
    if (!slot) {
        free(data);
        _snwprintf(msg, msg_len, L"the template DLL has no config block");
        return -1;
    }
    memcpy(slot, cfg, sizeof *cfg);
    memcpy(slot->magic, DEMAKE_CONFIG_MAGIC, sizeof slot->magic);
    slot->size = sizeof *slot;

    wchar_t path[MAX_PATH], backup[MAX_PATH + 32];
    dll_path(dir, path);
    demake_config existing;
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && demake_read(path, &existing) != 0) {
        if (!force) {
            free(data);
            _snwprintf(msg, msg_len, L"%s exists and was not written by AnyDemake (another mod?); use force to back it up and replace it", path);
            return -2;
        }
        _snwprintf(backup, MAX_PATH + 32, L"%s.anydemake-backup", path);
        if (!MoveFileExW(path, backup, MOVEFILE_REPLACE_EXISTING)) {
            free(data);
            _snwprintf(msg, msg_len, L"cannot back up %s", path);
            return -3;
        }
    }

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    BOOL ok = f != INVALID_HANDLE_VALUE && WriteFile(f, data, (DWORD)len, &written, NULL) && written == len;
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    free(data);
    if (!ok) {
        _snwprintf(msg, msg_len, L"cannot write %s (error %lu)", path, GetLastError());
        return -4;
    }
    _snwprintf(msg, msg_len, L"wrote %s", path);
    return 0;
}

int demake_remove(const wchar_t *dir, wchar_t *msg, size_t msg_len)
{
    wchar_t path[MAX_PATH], backup[MAX_PATH + 32];
    dll_path(dir, path);
    demake_config existing;
    if (demake_read(path, &existing) != 0) {
        _snwprintf(msg, msg_len, L"no AnyDemake d3d11.dll in %s", dir);
        return -1;
    }
    if (!DeleteFileW(path)) {
        _snwprintf(msg, msg_len, L"cannot delete %s (is the game running?)", path);
        return -2;
    }
    _snwprintf(backup, MAX_PATH + 32, L"%s.anydemake-backup", path);
    if (GetFileAttributesW(backup) != INVALID_FILE_ATTRIBUTES && MoveFileW(backup, path))
        _snwprintf(msg, msg_len, L"removed AnyDemake and restored the previous d3d11.dll");
    else
        _snwprintf(msg, msg_len, L"removed %s", path);
    return 0;
}
