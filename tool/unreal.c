#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tool-core.h"

#define BEGIN_MARK "; anydemake begin"
#define END_MARK "; anydemake end"

static int is_dir(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int has_exe(const wchar_t *dir)
{
    wchar_t pattern[MAX_PATH + 8];
    WIN32_FIND_DATAW fd;
    _snwprintf(pattern, MAX_PATH + 8, L"%s\\*.exe", dir);
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    FindClose(h);
    return 1;
}

static const wchar_t *const platforms[] = {L"Win64", L"WinGDK"};

/* A packaged Unreal game is <root>\Engine plus <root>\<Project>\Binaries\Win64\<Project>-Win64-Shipping.exe. */
int unreal_find(const wchar_t *dir, wchar_t *project, wchar_t *bin_dir)
{
    wchar_t full[MAX_PATH], probe[MAX_PATH + 64];
    if (!GetFullPathNameW(dir, MAX_PATH, full, NULL))
        return -1;
    size_t n = wcslen(full);
    while (n && (full[n - 1] == L'\\' || full[n - 1] == L'/'))
        full[--n] = 0;

    /* Given Binaries\Win64: walk up to <root>\<Project>. */
    wchar_t *p1 = wcsrchr(full, L'\\');
    if (p1) {
        for (int i = 0; i < 2; i++) {
            if (_wcsicmp(p1 + 1, platforms[i]))
                continue;
            wchar_t up[MAX_PATH];
            wcscpy(up, full);
            up[p1 - full] = 0;
            wchar_t *p2 = wcsrchr(up, L'\\');
            if (!p2 || _wcsicmp(p2 + 1, L"Binaries"))
                break;
            *p2 = 0;
            wchar_t *p3 = wcsrchr(up, L'\\');
            if (!p3)
                break;
            _snwprintf(probe, MAX_PATH + 64, L"%.*s\\Engine", (int)(p3 - up), up);
            if (!is_dir(probe))
                break;
            wcscpy(project, p3 + 1);
            wcscpy(bin_dir, full);
            return 0;
        }
    }

    /* Given the root: find the project folder next to Engine. */
    _snwprintf(probe, MAX_PATH + 64, L"%s\\Engine", full);
    if (!is_dir(probe))
        return -1;
    _snwprintf(probe, MAX_PATH + 64, L"%s\\*", full);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(probe, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    int found = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' ||
            !_wcsicmp(fd.cFileName, L"Engine"))
            continue;
        for (int i = 0; i < 2 && !found; i++) {
            _snwprintf(probe, MAX_PATH + 64, L"%s\\%s\\Binaries\\%s", full, fd.cFileName, platforms[i]);
            if (is_dir(probe) && has_exe(probe)) {
                wcscpy(project, fd.cFileName);
                wcsncpy(bin_dir, probe, MAX_PATH - 1);
                bin_dir[MAX_PATH - 1] = 0;
                found = 1;
            }
        }
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found ? 0 : -1;
}

/* UE5 saves to Windows, UE4 to WindowsNoEditor; write every one that exists, or both. */
static int ini_paths(const wchar_t *project, wchar_t paths[2][MAX_PATH], int create)
{
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return 0;
    static const wchar_t *const dirs[] = {L"Windows", L"WindowsNoEditor"};
    int count = 0, existing = 0;
    wchar_t dir[MAX_PATH + 64];
    for (int i = 0; i < 2; i++) {
        _snwprintf(dir, MAX_PATH + 64, L"%s\\%s\\Saved\\Config\\%s", base, project, dirs[i]);
        existing += is_dir(dir);
    }
    for (int i = 0; i < 2; i++) {
        _snwprintf(dir, MAX_PATH + 64, L"%s\\%s\\Saved\\Config\\%s", base, project, dirs[i]);
        if (existing && !is_dir(dir))
            continue;
        if (!existing && (!create || SHCreateDirectoryExW(NULL, dir, NULL) != ERROR_SUCCESS))
            continue;
        _snwprintf(paths[count++], MAX_PATH, L"%s\\Engine.ini", dir);
    }
    return count;
}

static char *read_text(const wchar_t *path, size_t *len)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    *len = 0;
    if (f == INVALID_HANDLE_VALUE)
        return calloc(1, 1);
    DWORD size = GetFileSize(f, NULL), got = 0;
    char *buf = size < 16 * 1024 * 1024 ? malloc(size + 1) : NULL;
    if (buf && ReadFile(f, buf, size, &got, NULL) && got == size) {
        buf[size] = 0;
        *len = size;
    } else {
        free(buf);
        buf = NULL;
    }
    CloseHandle(f);
    return buf;
}

/* Drops a previous AnyDemake block: 1 if one was there, 0 if not, -1 if its
   end marker is gone (edited by hand), in which case nothing is touched. */
static int strip_block(char *text, size_t *len)
{
    char *b = strstr(text, BEGIN_MARK);
    if (!b)
        return 0;
    char *e = strstr(b, END_MARK);
    if (!e)
        return -1;
    char *after = e + strlen(END_MARK);
    while (*after == '\r' || *after == '\n')
        after++;
    memmove(b, after, strlen(after) + 1);
    *len = strlen(text);
    return 1;
}

/* Writes a temporary file and moves it over the target, so a failed write
   never leaves a truncated Engine.ini. */
static int write_text(const wchar_t *path, const char *a, size_t alen, const char *b, size_t blen)
{
    wchar_t tmp[MAX_PATH + 16];
    _snwprintf(tmp, MAX_PATH + 16, L"%s.anydemake-tmp", path);
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    DWORD w1 = 0, w2 = 0;
    BOOL ok = WriteFile(f, a, (DWORD)alen, &w1, NULL) && WriteFile(f, b, (DWORD)blen, &w2, NULL) &&
              FlushFileBuffers(f);
    CloseHandle(f);
    ok = ok && w1 == alen && w2 == blen && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok)
        DeleteFileW(tmp);
    return ok;
}

static void backup_path(const wchar_t *path, wchar_t *out)
{
    _snwprintf(out, MAX_PATH + 32, L"%s.anydemake-backup", path);
}

static void add(char *out, size_t cap, const char *line)
{
    strncat(out, line, cap - strlen(out) - 1);
    strncat(out, "\r\n", cap - strlen(out) - 1);
}

/* Settings in [SystemSettings] outrank the game's scalability options. */
static void build_block(const demake_config *cfg, char *out, size_t cap)
{
    char line[96];
    int lowest = cfg->mip_skip >= 3;
    out[0] = 0;
    add(out, cap, BEGIN_MARK " (written by anydemake.exe; remove it there or delete these lines)");
    add(out, cap, "[SystemSettings]");
    if (cfg->render_scale && cfg->render_scale < 100) {
        snprintf(line, sizeof line, "r.ScreenPercentage=%u", cfg->render_scale);
        add(out, cap, line);
    }
    if (cfg->mip_skip) {
        snprintf(line, sizeof line, "r.MipMapLODBias=%u", cfg->mip_skip);
        add(out, cap, line);
        snprintf(line, sizeof line, "r.SkeletalMeshLODBias=%u", (cfg->mip_skip + 1) / 2);
        add(out, cap, line);
    }
    if (cfg->sampler_mode != DEMAKE_SAMPLER_KEEP)
        add(out, cap, "r.MaxAnisotropy=0");
    add(out, cap, "r.DynamicGlobalIlluminationMethod=0");
    add(out, cap, "r.ReflectionMethod=0");
    add(out, cap, "r.Shadow.Virtual.Enable=0");
    add(out, cap, "r.VolumetricFog=0");
    add(out, cap, "r.VolumetricCloud=0");
    add(out, cap, "r.MotionBlurQuality=0");
    add(out, cap, "r.DepthOfFieldQuality=0");
    add(out, cap, "r.AmbientOcclusionLevels=0");
    add(out, cap, "r.SSR.Quality=0");
    add(out, cap, "r.LensFlareQuality=0");
    add(out, cap, "r.RayTracing=0");
    add(out, cap, "r.StaticMeshLODDistanceScale=2");
    if (lowest) {
        add(out, cap, "r.ShadowQuality=1");
        add(out, cap, "r.Shadow.MaxResolution=512");
        add(out, cap, "r.ViewDistanceScale=0.5");
        add(out, cap, "r.Nanite.MaxPixelsPerEdge=4");
        add(out, cap, "foliage.DensityScale=0.4");
        add(out, cap, "grass.DensityScale=0.4");
        add(out, cap, "r.BloomQuality=1");
    }
    add(out, cap, END_MARK);
}

int unreal_write(const wchar_t *project, const demake_config *cfg, wchar_t *msg, size_t msg_len)
{
    wchar_t paths[2][MAX_PATH];
    int n = ini_paths(project, paths, 1);
    if (!n) {
        _snwprintf(msg, msg_len, L"cannot find or create the Engine.ini folder of %s", project);
        return -5;
    }
    char block[4096];
    build_block(cfg, block, sizeof block);
    for (int i = 0; i < n; i++) {
        size_t len;
        char *text = read_text(paths[i], &len);
        if (!text || (len >= 2 && (unsigned char)text[0] == 0xFF && (unsigned char)text[1] == 0xFE)) {
            free(text);
            _snwprintf(msg, msg_len, L"cannot read %s (UTF-16 files are not supported)", paths[i]);
            return -5;
        }
        if (strip_block(text, &len) < 0) {
            free(text);
            _snwprintf(msg, msg_len, L"%s has an AnyDemake block that was edited by hand; fix or delete it first",
                       paths[i]);
            return -5;
        }
        /* The game may rewrite this file and drop our markers; the first
           original is kept so Remove can still restore it. */
        wchar_t backup[MAX_PATH + 32];
        backup_path(paths[i], backup);
        if (GetFileAttributesW(paths[i]) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(backup) == INVALID_FILE_ATTRIBUTES && !CopyFileW(paths[i], backup, TRUE)) {
            free(text);
            _snwprintf(msg, msg_len, L"cannot back up %s", paths[i]);
            return -5;
        }
        int sep = len && text[len - 1] != '\n';
        char *joined = malloc(len + 3);
        memcpy(joined, text, len);
        if (sep)
            memcpy(joined + len, "\r\n", 2), len += 2;
        int ok = write_text(paths[i], joined, len, block, strlen(block));
        free(joined);
        free(text);
        if (!ok) {
            _snwprintf(msg, msg_len, L"cannot write %s", paths[i]);
            return -5;
        }
    }
    _snwprintf(msg, msg_len, L"Unreal game \"%s\": low settings in %s%s%s", project, paths[0], n > 1 ? L" and " : L"",
               n > 1 ? paths[1] : L"");
    return 0;
}

int unreal_remove(const wchar_t *project, wchar_t *msg, size_t msg_len)
{
    wchar_t paths[2][MAX_PATH];
    int n = ini_paths(project, paths, 0), removed = 0;
    for (int i = 0; i < n; i++) {
        size_t len;
        wchar_t backup[MAX_PATH + 32];
        backup_path(paths[i], backup);
        int has_backup = GetFileAttributesW(backup) != INVALID_FILE_ATTRIBUTES;
        char *text = read_text(paths[i], &len);
        int found = text ? strip_block(text, &len) : 0;
        if (found > 0 && write_text(paths[i], text, len, "", 0)) {
            removed++;
            if (has_backup)
                DeleteFileW(backup);
        } else if (found <= 0 && has_backup && MoveFileExW(backup, paths[i], MOVEFILE_REPLACE_EXISTING)) {
            /* Markers gone (the game rewrote the file, or the block was edited): restore the original. */
            removed++;
        }
        free(text);
    }
    if (!removed)
        return -1;
    _snwprintf(msg, msg_len, L"removed the AnyDemake settings from the Engine.ini of %s", project);
    return 0;
}
