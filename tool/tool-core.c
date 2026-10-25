#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tool-core.h"
#include "resource.h"

#define CFG(mip, def, min, sampler, pix, bits, scale)                                                                 \
    {DEMAKE_CONFIG_MAGIC, sizeof(demake_config), mip, def, min, sampler, pix, bits, 1, scale}

const demake_preset demake_presets[] = {
    {"off", L"Off (passthrough)", CFG(0, 0, 64, DEMAKE_SAMPLER_KEEP, 0, 0, 100)},
    {"light", L"Light: half-size textures", CFG(1, 0, 64, DEMAKE_SAMPLER_BILINEAR, 0, 0, 100)},
    {"ps2", L"PS2 look", CFG(2, 0, 64, DEMAKE_SAMPLER_BILINEAR, 2, 0, 75)},
    {"ps1", L"PS1 look", CFG(3, 0, 32, DEMAKE_SAMPLER_POINT, 3, 5, 50)},
    {"potato", L"Potato: lowest", CFG(4, 1, 16, DEMAKE_SAMPLER_POINT, 4, 4, 50)},
};
const int demake_preset_count = sizeof demake_presets / sizeof demake_presets[0];

static const struct {
    unsigned api;
    const wchar_t *file;
    int resource;
} proxies[] = {
    {DEMAKE_API_D3D11, L"d3d11.dll", IDR_PROXY_D3D11},
    {DEMAKE_API_D3D12, L"d3d12.dll", IDR_PROXY_D3D12},
};

void demake_options_default(demake_options *o)
{
    memset(o, 0, sizeof *o);
    o->cfg = demake_presets[0].cfg;
    o->cfg.log = 0;
    o->apis = DEMAKE_API_ALL;
    o->unreal = 1;
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
    for (size_t i = 0; i + offsetof(demake_config, render_scale) <= len; i++)
        if (!memcmp(data + i, DEMAKE_CONFIG_MAGIC, mlen))
            return (demake_config *)(data + i);
    return NULL;
}

static int read_one(const wchar_t *path, demake_config *cfg)
{
    size_t len;
    unsigned char *data = read_file(path, &len);
    if (!data)
        return -1;
    demake_config *found = find_config(data, len);
    if (found) {
        demake_options o;
        demake_options_default(&o);
        *cfg = o.cfg;
        /* Older DLLs have a shorter block; keep defaults for the missing fields. */
        size_t n = found->size < sizeof *cfg ? found->size : sizeof *cfg;
        if ((unsigned char *)found + n > data + len)
            n = data + len - (unsigned char *)found;
        memcpy(cfg, found, n);
    }
    free(data);
    return found ? 0 : -2;
}

/* The folder that gets the DLLs: the given folder, or Binaries\Win64 of an Unreal game root. */
static void target_dir(const wchar_t *dir, wchar_t *out, wchar_t *project)
{
    project[0] = 0;
    if (unreal_find(dir, project, out) != 0)
        wcsncpy(out, dir, MAX_PATH - 1), out[MAX_PATH - 1] = 0;
}

int demake_read(const wchar_t *path, demake_config *cfg, unsigned *apis)
{
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return read_one(path, cfg);
    wchar_t dir[MAX_PATH], project[MAX_PATH], file[MAX_PATH + 16];
    target_dir(path, dir, project);
    unsigned found = 0;
    for (int i = 1; i >= 0; i--) {
        _snwprintf(file, MAX_PATH + 16, L"%s\\%s", dir, proxies[i].file);
        if (read_one(file, cfg) == 0)
            found |= proxies[i].api;
    }
    if (apis)
        *apis = found;
    return found ? 0 : -2;
}

static int write_proxy(const wchar_t *path, int resource, const demake_config *cfg, int force, wchar_t *msg,
                       size_t msg_len)
{
    HRSRC res = FindResourceW(NULL, MAKEINTRESOURCEW(resource), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL h = res ? LoadResource(NULL, res) : NULL;
    const void *tmpl = h ? LockResource(h) : NULL;
    size_t len = res ? SizeofResource(NULL, res) : 0;
    unsigned char *data = tmpl && len ? malloc(len) : NULL;
    if (!data) {
        _snwprintf(msg, msg_len, L"the template DLL is missing from this executable");
        return -1;
    }
    memcpy(data, tmpl, len);
    demake_config *slot = find_config(data, len);
    if (!slot || slot->size != sizeof *slot) {
        free(data);
        _snwprintf(msg, msg_len, L"the template DLL has no config block");
        return -1;
    }
    memcpy(slot, cfg, sizeof *cfg);
    memcpy(slot->magic, DEMAKE_CONFIG_MAGIC, sizeof slot->magic);
    slot->size = sizeof *slot;

    wchar_t backup[MAX_PATH + 32];
    demake_config existing;
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && read_one(path, &existing) != 0) {
        if (!force) {
            free(data);
            _snwprintf(msg, msg_len, L"%s exists and was not written by AnyDemake (another mod?); use force to back it up and replace it", path);
            return DEMAKE_ERR_FOREIGN;
        }
        _snwprintf(backup, MAX_PATH + 32, L"%s.anydemake-backup", path);
        if (GetFileAttributesW(backup) != INVALID_FILE_ATTRIBUTES) {
            free(data);
            _snwprintf(msg, msg_len, L"%s already exists; move it away first so the older backup is not lost", backup);
            return -3;
        }
        if (!MoveFileW(path, backup)) {
            free(data);
            _snwprintf(msg, msg_len, L"cannot back up %s", path);
            return -3;
        }
    }

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    BOOL ok = f != INVALID_HANDLE_VALUE && WriteFile(f, data, (DWORD)len, &written, NULL) && written == len;
    DWORD err = GetLastError();
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    free(data);
    if (!ok) {
        _snwprintf(msg, msg_len, L"cannot write %s (error %lu; is the game running?)", path, err);
        return -4;
    }
    _snwprintf(msg, msg_len, L"wrote %s", path);
    return 0;
}

static void append(wchar_t *msg, size_t msg_len, const wchar_t *line)
{
    size_t n = wcslen(msg);
    if (n + 2 < msg_len)
        _snwprintf(msg + n, msg_len - n, n ? L"\n%s" : L"%s", line);
    msg[msg_len - 1] = 0;
}

int demake_install(const wchar_t *dir, const demake_options *o, wchar_t *msg, size_t msg_len)
{
    wchar_t bin[MAX_PATH], project[MAX_PATH], path[MAX_PATH + 16], line[MAX_PATH + 200];
    msg[0] = 0;
    target_dir(dir, bin, project);
    /* Check every target before writing any, so a refusal leaves nothing half-installed. */
    for (int i = 0; i < 2; i++) {
        demake_config existing;
        _snwprintf(path, MAX_PATH + 16, L"%s\\%s", bin, proxies[i].file);
        if ((o->apis & proxies[i].api) && !o->force && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES &&
            read_one(path, &existing) != 0) {
            _snwprintf(msg, msg_len, L"%s exists and was not written by AnyDemake (another mod?); use force to back it up and replace it", path);
            return DEMAKE_ERR_FOREIGN;
        }
    }
    int written[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        if (!(o->apis & proxies[i].api))
            continue;
        _snwprintf(path, MAX_PATH + 16, L"%s\\%s", bin, proxies[i].file);
        int rc = write_proxy(path, proxies[i].resource, &o->cfg, o->force, line, sizeof line / sizeof line[0]);
        append(msg, msg_len, line);
        if (rc) {
            /* Leave nothing half-installed: undo the DLLs written in this run. */
            for (int j = 0; j < i; j++)
                if (written[j]) {
                    _snwprintf(path, MAX_PATH + 16, L"%s\\%s", bin, proxies[j].file);
                    DeleteFileW(path);
                    wchar_t backup[MAX_PATH + 48];
                    _snwprintf(backup, MAX_PATH + 48, L"%s.anydemake-backup", path);
                    MoveFileW(backup, path);
                }
            return rc;
        }
        written[i] = 1;
    }
    if (project[0] && o->unreal) {
        int rc = unreal_write(project, &o->cfg, line, sizeof line / sizeof line[0]);
        append(msg, msg_len, line);
        if (rc)
            return rc;
    }
    return 0;
}

int demake_remove(const wchar_t *dir, wchar_t *msg, size_t msg_len)
{
    wchar_t bin[MAX_PATH], project[MAX_PATH], path[MAX_PATH + 16], backup[MAX_PATH + 48], line[MAX_PATH + 200];
    msg[0] = 0;
    target_dir(dir, bin, project);
    int removed = 0, failed = 0;
    for (int i = 0; i < 2; i++) {
        demake_config existing;
        _snwprintf(path, MAX_PATH + 16, L"%s\\%s", bin, proxies[i].file);
        if (read_one(path, &existing) != 0)
            continue;
        if (!DeleteFileW(path)) {
            _snwprintf(line, MAX_PATH + 200, L"cannot delete %s (is the game running?)", path);
            append(msg, msg_len, line);
            failed = 1;
            continue;
        }
        removed = 1;
        _snwprintf(backup, MAX_PATH + 48, L"%s.anydemake-backup", path);
        if (GetFileAttributesW(backup) != INVALID_FILE_ATTRIBUTES && MoveFileW(backup, path))
            _snwprintf(line, MAX_PATH + 200, L"removed %s and restored the previous one", path);
        else
            _snwprintf(line, MAX_PATH + 200, L"removed %s", path);
        append(msg, msg_len, line);
    }
    if (project[0] && unreal_remove(project, line, sizeof line / sizeof line[0]) == 0) {
        append(msg, msg_len, line);
        removed = 1;
    }
    if (!removed && !failed)
        _snwprintf(msg, msg_len, L"nothing from AnyDemake in %s", dir);
    return failed ? -2 : removed ? 0 : -1;
}
