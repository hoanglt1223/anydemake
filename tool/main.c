#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tool-core.h"

static const char *sampler_names[] = {"keep", "point", "bilinear"};

static int usage(void)
{
    fwprintf(stderr,
             L"usage: anydemake                       (opens the window)\n"
             L"       anydemake --out <game dir> [--preset off|light|ps2|ps1|potato]\n"
             L"                 [--mip-skip 0-4] [--mip-skip-default] [--min-size N]\n"
             L"                 [--sampler keep|point|bilinear] [--pixelate N] [--color-bits 0-8]\n"
             L"                 [--log] [--force]\n"
             L"       anydemake --read <game dir | dll>\n"
             L"       anydemake --remove <game dir>\n");
    return 2;
}

static void print_config(const demake_config *c)
{
    wprintf(L"mip_skip=%u\nmip_skip_default=%u\nmin_texture_size=%u\nsampler=%S\npixelate=%u\ncolor_bits=%u\nlog=%u\n",
            c->mip_skip, c->mip_skip_default, c->min_texture_size,
            c->sampler_mode < 3 ? sampler_names[c->sampler_mode] : "?", c->pixelate, c->color_bits, c->log);
}

static int parse_uint(const wchar_t *s, unsigned max, uint32_t *out)
{
    wchar_t *end;
    unsigned long v = wcstoul(s, &end, 10);
    if (!*s || *end || v > max)
        return 0;
    *out = (uint32_t)v;
    return 1;
}

static int run_cli(int argc, wchar_t **argv)
{
    demake_config cfg;
    demake_config_default(&cfg);
    const wchar_t *out = NULL;
    int force = 0;
    wchar_t msg[MAX_PATH + 200];

    for (int i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];
        const wchar_t *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!wcscmp(a, L"--read") && v) {
            if (demake_read(v, &cfg) != 0) {
                fwprintf(stderr, L"no AnyDemake config in %s\n", v);
                return 1;
            }
            print_config(&cfg);
            return 0;
        } else if (!wcscmp(a, L"--remove") && v) {
            int rc = demake_remove(v, msg, sizeof msg / sizeof msg[0]);
            fwprintf(rc ? stderr : stdout, L"%s\n", msg);
            return rc ? 1 : 0;
        } else if (!wcscmp(a, L"--out") && v) {
            out = argv[++i];
        } else if (!wcscmp(a, L"--preset") && v) {
            char name[32];
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, name, sizeof name, NULL, NULL);
            const demake_preset *p = demake_find_preset(name);
            if (!p)
                return usage();
            uint32_t log = cfg.log;
            cfg = p->cfg;
            cfg.log = log;
        } else if (!wcscmp(a, L"--mip-skip") && v) {
            if (!parse_uint(argv[++i], 4, &cfg.mip_skip))
                return usage();
        } else if (!wcscmp(a, L"--mip-skip-default")) {
            cfg.mip_skip_default = 1;
        } else if (!wcscmp(a, L"--min-size") && v) {
            if (!parse_uint(argv[++i], 16384, &cfg.min_texture_size))
                return usage();
        } else if (!wcscmp(a, L"--sampler") && v) {
            i++;
            int found = 0;
            for (uint32_t s = 0; s < 3; s++) {
                wchar_t w[16];
                MultiByteToWideChar(CP_UTF8, 0, sampler_names[s], -1, w, 16);
                if (!wcscmp(argv[i], w)) {
                    cfg.sampler_mode = s;
                    found = 1;
                }
            }
            if (!found)
                return usage();
        } else if (!wcscmp(a, L"--pixelate") && v) {
            if (!parse_uint(argv[++i], 16, &cfg.pixelate))
                return usage();
        } else if (!wcscmp(a, L"--color-bits") && v) {
            if (!parse_uint(argv[++i], 8, &cfg.color_bits))
                return usage();
        } else if (!wcscmp(a, L"--log")) {
            cfg.log = 1;
        } else if (!wcscmp(a, L"--force")) {
            force = 1;
        } else {
            return usage();
        }
    }
    if (!out)
        return usage();
    int rc = demake_install(out, &cfg, force, msg, sizeof msg / sizeof msg[0]);
    fwprintf(rc ? stderr : stdout, L"%s\n", msg);
    return rc ? 1 : 0;
}

/* ---- window ------------------------------------------------------------ */

enum {
    ID_PRESET = 100, ID_MIP, ID_MIP_DEFAULT, ID_MIN, ID_SAMPLER, ID_PIXELATE, ID_BITS, ID_LOG,
    ID_DIR, ID_BROWSE, ID_INSTALL, ID_REMOVE, ID_LOAD, ID_STATUS,
};

static HWND wnd, ctl[ID_STATUS + 1];
static HFONT font;
static const unsigned pixelate_values[] = {0, 2, 3, 4, 6, 8};
static const unsigned bits_values[] = {0, 6, 5, 4, 3, 2};
static const unsigned min_values[] = {16, 32, 64, 128, 256};

static HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, wnd, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    if (id)
        ctl[id] = c;
    return c;
}

static void label(const wchar_t *text, int y)
{
    add(L"STATIC", text, 0, 16, y + 3, 200, 20, 0);
}

static HWND combo(int id, int y)
{
    return add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 220, y, 260, 200, id);
}

static void combo_add(int id, const wchar_t *text)
{
    SendMessageW(ctl[id], CB_ADDSTRING, 0, (LPARAM)text);
}

static int index_of(const unsigned *values, int n, unsigned v)
{
    for (int i = 0; i < n; i++)
        if (values[i] == v)
            return i;
    return 0;
}

#define COUNT(a) ((int)(sizeof a / sizeof a[0]))

static void show_config(const demake_config *c)
{
    SendMessageW(ctl[ID_MIP], CB_SETCURSEL, c->mip_skip, 0);
    SendMessageW(ctl[ID_MIP_DEFAULT], BM_SETCHECK, c->mip_skip_default ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(ctl[ID_MIN], CB_SETCURSEL, index_of(min_values, COUNT(min_values), c->min_texture_size), 0);
    SendMessageW(ctl[ID_SAMPLER], CB_SETCURSEL, c->sampler_mode, 0);
    SendMessageW(ctl[ID_PIXELATE], CB_SETCURSEL, index_of(pixelate_values, COUNT(pixelate_values), c->pixelate), 0);
    SendMessageW(ctl[ID_BITS], CB_SETCURSEL, index_of(bits_values, COUNT(bits_values), c->color_bits), 0);
    SendMessageW(ctl[ID_LOG], BM_SETCHECK, c->log ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void read_controls(demake_config *c)
{
    demake_config_default(c);
    c->mip_skip = (uint32_t)SendMessageW(ctl[ID_MIP], CB_GETCURSEL, 0, 0);
    c->mip_skip_default = SendMessageW(ctl[ID_MIP_DEFAULT], BM_GETCHECK, 0, 0) == BST_CHECKED;
    c->min_texture_size = min_values[SendMessageW(ctl[ID_MIN], CB_GETCURSEL, 0, 0)];
    c->sampler_mode = (uint32_t)SendMessageW(ctl[ID_SAMPLER], CB_GETCURSEL, 0, 0);
    c->pixelate = pixelate_values[SendMessageW(ctl[ID_PIXELATE], CB_GETCURSEL, 0, 0)];
    c->color_bits = bits_values[SendMessageW(ctl[ID_BITS], CB_GETCURSEL, 0, 0)];
    c->log = SendMessageW(ctl[ID_LOG], BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void status(const wchar_t *text)
{
    SetWindowTextW(ctl[ID_STATUS], text);
}

static int get_dir(wchar_t *dir)
{
    GetWindowTextW(ctl[ID_DIR], dir, MAX_PATH);
    DWORD attr = GetFileAttributesW(dir);
    if (!*dir || attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        status(L"Choose the folder that holds the game's .exe first.");
        return 0;
    }
    return 1;
}

static void on_command(int id, int code)
{
    wchar_t dir[MAX_PATH], msg[MAX_PATH + 200];
    demake_config c;
    switch (id) {
    case ID_PRESET:
        if (code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(ctl[ID_PRESET], CB_GETCURSEL, 0, 0);
            demake_config p = demake_presets[i].cfg;
            p.log = SendMessageW(ctl[ID_LOG], BM_GETCHECK, 0, 0) == BST_CHECKED;
            show_config(&p);
        }
        break;
    case ID_BROWSE: {
        BROWSEINFOW bi = {0};
        bi.hwndOwner = wnd;
        bi.lpszTitle = L"Folder with the game's .exe";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (pidl && SHGetPathFromIDListW(pidl, dir))
            SetWindowTextW(ctl[ID_DIR], dir);
        if (pidl)
            CoTaskMemFree(pidl);
        break;
    }
    case ID_INSTALL: {
        if (!get_dir(dir))
            break;
        read_controls(&c);
        int rc = demake_install(dir, &c, 0, msg, COUNT(msg));
        if (rc == -2 && MessageBoxW(wnd,
                                    L"This folder already has a d3d11.dll from another mod or tool.\n\n"
                                    L"Back it up (d3d11.dll.anydemake-backup) and replace it?",
                                    L"AnyDemake", MB_YESNO | MB_ICONWARNING) == IDYES)
            rc = demake_install(dir, &c, 1, msg, COUNT(msg));
        status(msg);
        break;
    }
    case ID_REMOVE:
        if (get_dir(dir)) {
            demake_remove(dir, msg, COUNT(msg));
            status(msg);
        }
        break;
    case ID_LOAD:
        if (get_dir(dir)) {
            if (demake_read(dir, &c) == 0) {
                show_config(&c);
                status(L"Loaded the settings of the installed d3d11.dll.");
            } else {
                status(L"No AnyDemake d3d11.dll in this folder.");
            }
        }
        break;
    }
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COMMAND:
        on_command(LOWORD(w), HIWORD(w));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int run_gui(void)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    NONCLIENTMETRICSW ncm = {sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"anydemake";
    RegisterClassW(&wc);
    RECT r = {0, 0, 500, 470};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    wnd = CreateWindowExW(0, wc.lpszClassName, L"AnyDemake", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                          r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);

    int y = 16;
    label(L"Game folder (with the .exe):", y);
    add(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 16, y + 24, 380, 24, ID_DIR);
    add(L"BUTTON", L"Browse...", WS_TABSTOP, 404, y + 24, 76, 24, ID_BROWSE);
    y += 64;
    label(L"Preset:", y);
    combo(ID_PRESET, y);
    for (int i = 0; i < demake_preset_count; i++)
        combo_add(ID_PRESET, demake_presets[i].label);
    y += 40;
    label(L"Texture size:", y);
    combo(ID_MIP, y);
    combo_add(ID_MIP, L"Full");
    combo_add(ID_MIP, L"1/2 (1/4 VRAM)");
    combo_add(ID_MIP, L"1/4 (1/16 VRAM)");
    combo_add(ID_MIP, L"1/8");
    combo_add(ID_MIP, L"1/16");
    y += 32;
    add(L"BUTTON", L"Also shrink streamed textures (riskier)", BS_AUTOCHECKBOX | WS_TABSTOP, 220, y, 270, 22,
        ID_MIP_DEFAULT);
    y += 30;
    label(L"Never shrink below:", y);
    combo(ID_MIN, y);
    for (int i = 0; i < COUNT(min_values); i++) {
        wchar_t t[16];
        _snwprintf(t, 16, L"%u px", min_values[i]);
        combo_add(ID_MIN, t);
    }
    y += 32;
    label(L"Texture filtering:", y);
    combo(ID_SAMPLER, y);
    combo_add(ID_SAMPLER, L"Keep the game's");
    combo_add(ID_SAMPLER, L"Pixelated (point)");
    combo_add(ID_SAMPLER, L"Bilinear, no anisotropic");
    y += 32;
    label(L"Pixelate screen:", y);
    combo(ID_PIXELATE, y);
    combo_add(ID_PIXELATE, L"Off");
    for (int i = 1; i < COUNT(pixelate_values); i++) {
        wchar_t t[32];
        _snwprintf(t, 32, L"%ux%u blocks", pixelate_values[i], pixelate_values[i]);
        combo_add(ID_PIXELATE, t);
    }
    y += 32;
    label(L"Colours:", y);
    combo(ID_BITS, y);
    combo_add(ID_BITS, L"Full");
    for (int i = 1; i < COUNT(bits_values); i++) {
        wchar_t t[48];
        _snwprintf(t, 48, L"%u bits per channel (%u colours)", bits_values[i], 1u << (3 * bits_values[i]));
        combo_add(ID_BITS, t);
    }
    y += 32;
    add(L"BUTTON", L"Write anydemake.log next to the game", BS_AUTOCHECKBOX | WS_TABSTOP, 220, y, 270, 22, ID_LOG);
    y += 36;
    add(L"BUTTON", L"Install d3d11.dll", BS_DEFPUSHBUTTON | WS_TABSTOP, 16, y, 150, 28, ID_INSTALL);
    add(L"BUTTON", L"Load installed", WS_TABSTOP, 174, y, 150, 28, ID_LOAD);
    add(L"BUTTON", L"Remove", WS_TABSTOP, 332, y, 148, 28, ID_REMOVE);
    y += 40;
    add(L"STATIC", L"DirectX 11 games only for now. Offline games only: games with anti-cheat "
                   L"(Vanguard, EasyAntiCheat, BattlEye) may ban accounts for a foreign DLL. "
                   L"Pixelate and colours change the look only; texture size and filtering save GPU work.",
        0, 16, y, 464, 56, 0);
    add(L"STATIC", L"", SS_SUNKEN | SS_LEFTNOWORDWRAP, 16, 470 - 30, 464, 22, ID_STATUS);

    SendMessageW(ctl[ID_PRESET], CB_SETCURSEL, 0, 0);
    demake_config c;
    demake_config_default(&c);
    show_config(&c);
    ShowWindow(wnd, SW_SHOWNORMAL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
        return run_gui();
    /* Built for the GUI subsystem; CLI output goes to redirected handles or the parent console. */
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((!out || out == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    return run_cli(argc, argv);
}
