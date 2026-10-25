# Testing a game

The automated tests run on WARP (a software GPU) and prove that each option changes what reaches DirectX. Only a real game on a real GPU shows whether it helps. This is the procedure.

## Pick games

Start with offline games without anti-cheat. Good first candidates:

- A DirectX 11 game (most games from 2012 to 2019): every option applies.
- An Unreal Engine game (Black Myth: Wukong, most UE4/UE5 games): the `Engine.ini` settings apply, plus filtering and the look options.
- A DirectX 12 game on another engine (Street Fighter 6): filtering and the look options only.

To see which API a game uses, install with **Log** on and start the game: `potatomode-d3d11.log` or `potatomode-d3d12.log` appears next to the DLLs (both may appear; the one with fps lines is the one that draws).

## Measure

1. Install with preset **Off** and **Log** ticked. Play the same scene for about a minute and quit.
2. Copy the log somewhere (it is overwritten on the next start).
3. Install again with a preset (for example **PS1**) and **Log** ticked. Play the same scene and quit.
4. Compare the `fps` and `VRAM` lines of the two logs.

Write down the GPU, the in-game resolution and graphics settings: they must be the same in both runs.

## What to look for

| Symptom | Likely cause | Try |
|---|---|---|
| Only a corner or part of the picture shows | The game reads screen buffers by pixel position | 3D render resolution back to Full |
| Black or flickering picture | Render target scaling or the post-process | Render resolution Full, then pixelate and colours Off |
| Blurry text in menus | Texture size on UI textures | Raise "Never shrink below" or set texture size to Full |
| Game does not start | The proxy cannot load, or anti-cheat refused it | Send the log; press Remove |
| No change at all | The game uses Vulkan or OpenGL, or loads DirectX from elsewhere | Send the log; check that it was created |

## Report

Add a file to `compat/` (copy `compat/_template.toml`) with the measurements and what you saw, or send the two logs and a screenshot of each run.
