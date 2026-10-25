First public preview. Download the zip, run `potatify.exe`, pick a game folder and a preset, press Install.

**Experimental:** the automated tests run on a software GPU (WARP). Potatify has not been measured on real games yet, and reports are welcome. `TESTING.md` explains how: run once with the `Off` preset and the log on, once with a preset, and compare the fps/VRAM lines.

- `d3d11.dll` proxy: lower 3D render resolution, smaller textures, cheaper filtering, pixelate and colour reduction.
- `d3d12.dll` proxy: filtering (including static samplers), pixelate and colour reduction.
- Unreal Engine games: lower settings written to `Engine.ini`, with a backup that Remove restores.
- Offline games only: games with anti-cheat may ban accounts for a foreign DLL.
