# AnyDemake

Make heavy PC games lighter by dropping one `d3d11.dll` next to the game's `.exe`. The DLL sits between the game and DirectX and rewrites graphics commands on the way through: smaller textures, cheaper texture filtering, and an optional retro look. Game logic is untouched, so gameplay stays the same.

## Use

1. Run `anydemake.exe`, choose the folder with the game's `.exe`, pick a preset or tick options, and press **Install d3d11.dll**.
2. Start the game. **Remove** deletes the DLL (and restores a `d3d11.dll` from another mod if one was backed up).

| Option | Saves | What it does |
|---|---|---|
| Texture size | VRAM, bandwidth | Drops the top mip levels of textures the game uploads (1/2 size = 1/4 memory) |
| Texture filtering | GPU time | Point or plain bilinear instead of anisotropic filtering |
| Pixelate screen, colours | nothing (look only) | Blocky pixels and a reduced palette at Present |

CLI, for scripts:

```
anydemake --out <game dir> [--preset off|light|ps2|ps1|potato] [--mip-skip 0-4] [--mip-skip-default]
          [--min-size N] [--sampler keep|point|bilinear] [--pixelate N] [--color-bits 0-8] [--log] [--force]
anydemake --read <game dir | dll>
anydemake --remove <game dir>
```

## Limits

- DirectX 11 games only for now; DX12 (`dxgi.dll`/`d3d12.dll`) and lower render resolution are planned.
- Offline games only. Games with anti-cheat (Vanguard, EasyAntiCheat, BattlEye) may ban accounts for a foreign DLL.
- It cannot make CPU-heavy games faster or add GPU features the card lacks.

## Build and test

```
cmake -S . -B out/msvc -G "Visual Studio 17 2022" -A x64
cmake --build out/msvc --config Release
ctest --test-dir out/msvc -C Release      # headless: WARP device, hidden window
```

`anydemake.exe` embeds the template proxy DLL as a resource and patches its config block (`proxy/demake-config.h`) when it writes a game's `d3d11.dll`.
