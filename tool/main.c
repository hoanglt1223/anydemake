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
             L"usage: potatify                       (opens the window)\n"
             L"       potatify --out <game dir> [--preset off|light|ps2|ps1|potato]\n"
             L"                 [--mip-skip 0-4] [--mip-skip-default] [--min-size N]\n"
             L"                 [--sampler keep|point|bilinear] [--pixelate N] [--color-bits 0-8]\n"
             L"                 [--render-scale 25-100] [--api d3d11|d3d12|both] [--unreal auto|off]\n"
             L"                 [--log] [--force]\n"
             L"       potatify --read <game dir | dll>\n"
             L"       potatify --remove <game dir>\n");
    return 2;
}

static void print_config(const demake_config *c, unsigned apis)
{
    if (apis)
        wprintf(L"installed=%s%s\n", apis & DEMAKE_API_D3D11 ? L"d3d11 " : L"", apis & DEMAKE_API_D3D12 ? L"d3d12" : L"");
    wprintf(L"mip_skip=%u\nmip_skip_default=%u\nmin_texture_size=%u\nsampler=%S\npixelate=%u\ncolor_bits=%u\n"
            L"render_scale=%u\nlog=%u\n",
            c->mip_skip, c->mip_skip_default, c->min_texture_size,
            c->sampler_mode < 3 ? sampler_names[c->sampler_mode] : "?", c->pixelate, c->color_bits, c->render_scale,
            c->log);
}

static int parse_uint(const wchar_t *s, unsigned lo, unsigned hi, uint32_t *out)
{
    wchar_t *end;
    unsigned long v = wcstoul(s, &end, 10);
    if (!*s || *end || v < lo || v > hi)
        return 0;
    *out = (uint32_t)v;
    return 1;
}

static int run_cli(int argc, wchar_t **argv)
{
    demake_options o;
    demake_options_default(&o);
    const wchar_t *out = NULL;
    wchar_t msg[4 * MAX_PATH + 800];

    for (int i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];
        const wchar_t *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!wcscmp(a, L"--read") && v) {
            unsigned apis = 0;
            if (demake_read(v, &o.cfg, &apis) != 0) {
                fwprintf(stderr, L"no Potatify config in %s\n", v);
                return 1;
            }
            print_config(&o.cfg, apis);
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
            uint32_t log = o.cfg.log;
            o.cfg = p->cfg;
            o.cfg.log = log;
        } else if (!wcscmp(a, L"--mip-skip") && v) {
            if (!parse_uint(argv[++i], 0, 4, &o.cfg.mip_skip))
                return usage();
        } else if (!wcscmp(a, L"--mip-skip-default")) {
            o.cfg.mip_skip_default = 1;
        } else if (!wcscmp(a, L"--min-size") && v) {
            if (!parse_uint(argv[++i], 1, 16384, &o.cfg.min_texture_size))
                return usage();
        } else if (!wcscmp(a, L"--sampler") && v) {
            i++;
            int found = 0;
            for (uint32_t s = 0; s < 3; s++) {
                wchar_t w[16];
                MultiByteToWideChar(CP_UTF8, 0, sampler_names[s], -1, w, 16);
                if (!wcscmp(argv[i], w)) {
                    o.cfg.sampler_mode = s;
                    found = 1;
                }
            }
            if (!found)
                return usage();
        } else if (!wcscmp(a, L"--pixelate") && v) {
            if (!parse_uint(argv[++i], 0, 16, &o.cfg.pixelate))
                return usage();
        } else if (!wcscmp(a, L"--color-bits") && v) {
            if (!parse_uint(argv[++i], 0, 8, &o.cfg.color_bits))
                return usage();
        } else if (!wcscmp(a, L"--render-scale") && v) {
            if (!parse_uint(argv[++i], 25, 100, &o.cfg.render_scale))
                return usage();
        } else if (!wcscmp(a, L"--api") && v) {
            i++;
            if (!wcscmp(argv[i], L"d3d11"))
                o.apis = DEMAKE_API_D3D11;
            else if (!wcscmp(argv[i], L"d3d12"))
                o.apis = DEMAKE_API_D3D12;
            else if (!wcscmp(argv[i], L"both"))
                o.apis = DEMAKE_API_ALL;
            else
                return usage();
        } else if (!wcscmp(a, L"--unreal") && v) {
            i++;
            if (!wcscmp(argv[i], L"auto"))
                o.unreal = 1;
            else if (!wcscmp(argv[i], L"off"))
                o.unreal = 0;
            else
                return usage();
        } else if (!wcscmp(a, L"--log")) {
            o.cfg.log = 1;
        } else if (!wcscmp(a, L"--force")) {
            o.force = 1;
        } else {
            return usage();
        }
    }
    if (!out)
        return usage();
    int rc = demake_install(out, &o, msg, sizeof msg / sizeof msg[0]);
    fwprintf(rc ? stderr : stdout, L"%s\n", msg);
    return rc ? 1 : 0;
}

/* ---- window ------------------------------------------------------------ */

enum {
    ID_PRESET = 100, ID_MIP, ID_MIP_DEFAULT, ID_MIN, ID_SAMPLER, ID_PIXELATE, ID_BITS, ID_SCALE, ID_LOG,
    ID_D3D11, ID_D3D12, ID_UNREAL, ID_DIR, ID_BROWSE, ID_INSTALL, ID_REMOVE, ID_LOAD, ID_STATUS,
};

#define WIN_W 520
#define WIN_H 620

static HWND wnd, ctl[ID_STATUS + 1];
static HFONT font;
static const unsigned pixelate_values[] = {0, 2, 3, 4, 6, 8};
static const unsigned bits_values[] = {0, 6, 5, 4, 3, 2};
static const unsigned min_values[] = {16, 32, 64, 128, 256};
static const unsigned scale_values[] = {100, 75, 67, 50, 33, 25};

#define COUNT(a) ((int)(sizeof a / sizeof a[0]))

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

static void combo(int id, int y)
{
    add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 220, y, 284, 240, id);
}

static void combo_add(int id, const wchar_t *text)
{
    SendMessageW(ctl[id], CB_ADDSTRING, 0, (LPARAM)text);
}

static void check(int id, const wchar_t *text, int x, int y, int w)
{
    add(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 22, id);
}

static int index_of(const unsigned *values, int n, unsigned v)
{
    for (int i = 0; i < n; i++)
        if (values[i] == v)
            return i;
    return 0;
}

static void set_check(int id, int on)
{
    SendMessageW(ctl[id], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

static int get_check(int id)
{
    return SendMessageW(ctl[id], BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static unsigned get_sel(int id)
{
    LRESULT i = SendMessageW(ctl[id], CB_GETCURSEL, 0, 0);
    return i < 0 ? 0 : (unsigned)i;
}

static void show_config(const demake_config *c)
{
    SendMessageW(ctl[ID_MIP], CB_SETCURSEL, c->mip_skip, 0);
    set_check(ID_MIP_DEFAULT, c->mip_skip_default);
    SendMessageW(ctl[ID_MIN], CB_SETCURSEL, index_of(min_values, COUNT(min_values), c->min_texture_size), 0);
    SendMessageW(ctl[ID_SAMPLER], CB_SETCURSEL, c->sampler_mode, 0);
    SendMessageW(ctl[ID_PIXELATE], CB_SETCURSEL, index_of(pixelate_values, COUNT(pixelate_values), c->pixelate), 0);
    SendMessageW(ctl[ID_BITS], CB_SETCURSEL, index_of(bits_values, COUNT(bits_values), c->color_bits), 0);
    SendMessageW(ctl[ID_SCALE], CB_SETCURSEL, index_of(scale_values, COUNT(scale_values), c->render_scale), 0);
    set_check(ID_LOG, c->log);
}

static void read_controls(demake_options *o)
{
    demake_options_default(o);
    demake_config *c = &o->cfg;
    c->mip_skip = get_sel(ID_MIP);
    c->mip_skip_default = get_check(ID_MIP_DEFAULT);
    c->min_texture_size = min_values[get_sel(ID_MIN)];
    c->sampler_mode = get_sel(ID_SAMPLER);
    c->pixelate = pixelate_values[get_sel(ID_PIXELATE)];
    c->color_bits = bits_values[get_sel(ID_BITS)];
    c->render_scale = scale_values[get_sel(ID_SCALE)];
    c->log = get_check(ID_LOG);
    o->apis = (get_check(ID_D3D11) ? DEMAKE_API_D3D11 : 0) | (get_check(ID_D3D12) ? DEMAKE_API_D3D12 : 0);
    o->unreal = get_check(ID_UNREAL);
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
    wchar_t dir[MAX_PATH], msg[4 * MAX_PATH + 800];
    demake_options o;
    switch (id) {
    case ID_PRESET:
        if (code == CBN_SELCHANGE) {
            demake_config p = demake_presets[get_sel(ID_PRESET)].cfg;
            p.log = get_check(ID_LOG);
            show_config(&p);
        }
        break;
    case ID_BROWSE: {
        BROWSEINFOW bi = {0};
        bi.hwndOwner = wnd;
        bi.lpszTitle = L"Folder with the game's .exe (for Unreal games the root folder also works)";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (pidl && SHGetPathFromIDListW(pidl, dir)) {
            SetWindowTextW(ctl[ID_DIR], dir);
            wchar_t project[MAX_PATH], bin[MAX_PATH];
            if (unreal_find(dir, project, bin) == 0) {
                _snwprintf(msg, COUNT(msg), L"Unreal Engine game \"%s\"; DLLs go to %s", project, bin);
                status(msg);
            } else {
                status(L"");
            }
        }
        if (pidl)
            CoTaskMemFree(pidl);
        break;
    }
    case ID_INSTALL: {
        if (!get_dir(dir))
            break;
        read_controls(&o);
        if (!o.apis) {
            status(L"Tick DirectX 11, DirectX 12, or both.");
            break;
        }
        int rc = demake_install(dir, &o, msg, COUNT(msg));
        if (rc == DEMAKE_ERR_FOREIGN &&
            MessageBoxW(wnd,
                        L"This folder already has a d3d11.dll or d3d12.dll from another mod or tool.\n\n"
                        L"Back it up (.potatify-backup) and replace it? Remove puts it back.",
                        L"Potatify", MB_YESNO | MB_ICONWARNING) == IDYES) {
            o.force = 1;
            rc = demake_install(dir, &o, msg, COUNT(msg));
        }
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
            unsigned apis = 0;
            if (demake_read(dir, &o.cfg, &apis) == 0) {
                show_config(&o.cfg);
                set_check(ID_D3D11, apis & DEMAKE_API_D3D11);
                set_check(ID_D3D12, apis & DEMAKE_API_D3D12);
                status(L"Loaded the settings of the installed DLLs.");
            } else {
                status(L"No Potatify DLL in this folder.");
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
    wc.lpszClassName = L"potatify";
    RegisterClassW(&wc);
    RECT r = {0, 0, WIN_W, WIN_H};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    wnd = CreateWindowExW(0, wc.lpszClassName, L"Potatify", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                          r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);

    wchar_t t[64];
    int y = 16;
    label(L"Game folder (with the .exe):", y);
    add(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 16, y + 24, 404, 24, ID_DIR);
    add(L"BUTTON", L"Browse...", WS_TABSTOP, 428, y + 24, 76, 24, ID_BROWSE);
    y += 62;
    label(L"Install for:", y);
    check(ID_D3D11, L"DirectX 11", 220, y, 100);
    check(ID_D3D12, L"DirectX 12", 330, y, 100);
    y += 26;
    check(ID_UNREAL, L"Unreal Engine games: also lower Engine.ini settings", 220, y, 290);
    y += 34;
    label(L"Preset:", y);
    combo(ID_PRESET, y);
    for (int i = 0; i < demake_preset_count; i++)
        combo_add(ID_PRESET, demake_presets[i].label);
    y += 40;
    label(L"3D render resolution:", y);
    combo(ID_SCALE, y);
    for (int i = 0; i < COUNT(scale_values); i++) {
        _snwprintf(t, COUNT(t), i ? L"%u%% of the screen" : L"Full (%u%%)", scale_values[i]);
        combo_add(ID_SCALE, t);
    }
    y += 32;
    label(L"Texture size:", y);
    combo(ID_MIP, y);
    combo_add(ID_MIP, L"Full");
    combo_add(ID_MIP, L"1/2 (1/4 VRAM)");
    combo_add(ID_MIP, L"1/4 (1/16 VRAM)");
    combo_add(ID_MIP, L"1/8");
    combo_add(ID_MIP, L"1/16");
    y += 30;
    check(ID_MIP_DEFAULT, L"Also shrink streamed textures (riskier)", 220, y, 290);
    y += 28;
    label(L"Never shrink below:", y);
    combo(ID_MIN, y);
    for (int i = 0; i < COUNT(min_values); i++) {
        _snwprintf(t, COUNT(t), L"%u px", min_values[i]);
        combo_add(ID_MIN, t);
    }
    y += 32;
    label(L"Texture filtering:", y);
    combo(ID_SAMPLER, y);
    combo_add(ID_SAMPLER, L"Keep the game's");
    combo_add(ID_SAMPLER, L"Pixelated (point)");
    combo_add(ID_SAMPLER, L"Bilinear, no anisotropic");
    y += 32;
    label(L"Pixelate screen (look only):", y);
    combo(ID_PIXELATE, y);
    combo_add(ID_PIXELATE, L"Off");
    for (int i = 1; i < COUNT(pixelate_values); i++) {
        _snwprintf(t, COUNT(t), L"%ux%u blocks", pixelate_values[i], pixelate_values[i]);
        combo_add(ID_PIXELATE, t);
    }
    y += 32;
    label(L"Colours (look only):", y);
    combo(ID_BITS, y);
    combo_add(ID_BITS, L"Full");
    for (int i = 1; i < COUNT(bits_values); i++) {
        _snwprintf(t, COUNT(t), L"%u bits per channel (%u colours)", bits_values[i], 1u << (3 * bits_values[i]));
        combo_add(ID_BITS, t);
    }
    y += 32;
    check(ID_LOG, L"Write potatify-d3d1x.log next to the game", 220, y, 290);
    y += 34;
    add(L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, 16, y, 156, 28, ID_INSTALL);
    add(L"BUTTON", L"Load installed", WS_TABSTOP, 182, y, 156, 28, ID_LOAD);
    add(L"BUTTON", L"Remove", WS_TABSTOP, 348, y, 156, 28, ID_REMOVE);
    y += 40;
    add(L"STATIC", L"Offline games only: games with anti-cheat (Vanguard, EasyAntiCheat, BattlEye) may ban "
                   L"accounts for a foreign DLL. Resolution, texture size and filtering save GPU work; pixelate and "
                   L"colours change the look only. If a game shows only part of the picture, set the render "
                   L"resolution back to Full.",
        0, 16, y, 488, 72, 0);
    add(L"STATIC", L"", SS_SUNKEN | SS_LEFT, 16, WIN_H - 58, 488, 46, ID_STATUS);

    SendMessageW(ctl[ID_PRESET], CB_SETCURSEL, 0, 0);
    demake_options o;
    demake_options_default(&o);
    show_config(&o.cfg);
    set_check(ID_D3D11, 1);
    set_check(ID_D3D12, 1);
    set_check(ID_UNREAL, 1);
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
