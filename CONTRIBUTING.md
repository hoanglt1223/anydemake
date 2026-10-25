# Contributing to Potatify

Thanks for helping. The most useful contribution right now is a **game report**: the automated tests only prove the
options reach DirectX on a software GPU, so real games on real (weak) GPUs decide what works.

## Report a game

1. Follow [docs/TESTING.md](docs/TESTING.md): one run with preset **Off**, one with a preset, both with **Log** on.
2. Either open a [game report issue](https://github.com/hoanglt1223/potatify/issues/new?template=game-report.yml)
   with both logs attached, or add `compat/<game-slug>.toml` (copy [compat/_template.toml](compat/_template.toml))
   in a pull request.

Please test offline games only. Do not use Potatify with online games that have anti-cheat: injecting a DLL can get an
account banned.

## Report a bug or ask for a feature

Use the issue templates. For a bug, attach `potatify-d3d11.log` / `potatify-d3d12.log` from the game folder and say
which options were ticked.

## Change the code

```
cmake -S . -B out/msvc -G "Visual Studio 17 2022" -A x64
cmake --build out/msvc --config Release
ctest --test-dir out/msvc -C Release
```

- Visual Studio 2022 (MSVC, x64) and CMake 3.24+. Everything is C; the proxy and tool link the static CRT.
- `proxy/` holds the `d3d11.dll` / `d3d12.dll` proxies, `tool/` the configurator that patches the config block into
  them, `tests/` the headless probes (WARP devices, hidden windows).
- Every new option needs a probe in `tests/` that fails without it. All `ctest` cases must pass; CI runs them on
  `windows-2022`.
- The config struct in `proxy/demake-config.h` is a binary contract between the tool and the DLLs: append fields,
  never reorder or remove them.
- Never commit game files, game executables, or anything extracted from a game.
- Keep commits small and use [conventional commit](https://www.conventionalcommits.org/) messages
  (`feat:`, `fix:`, `docs:`, ...).

By contributing you agree that your contribution is licensed under the [MIT license](LICENSE).
