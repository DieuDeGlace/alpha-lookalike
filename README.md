# alpha-lookalike

A standalone Steam Cube World color-palette mod for CubeModLoader API 7.3. Made by DieuDeGlace with the aid of AI (OpenAI ChatGPT/Codex). MIT licensed; SDK attribution in THIRD_PARTY_NOTICES.md.

The supplied Alpha/Omega screenshots guide a softer mint-green highlight palette, cooler teal shade, restrained desaturation and warm golden paths. This is an approximation; differences in game lighting, biome materials and existing effects prevent an exact screenshot match. Palette grading cannot create geometry shadows.

## Build

Windows x64, Visual Studio 2022 C++ Build Tools, Windows SDK, LLVM clang-cl, CMake 3.20+ and Ninja required. In an x64 Native Tools prompt:

```bat
git clone https://github.com/DieuDeGlace/alpha-lookalike.git
cd alpha-lookalike
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

## Install and settings

Close Steam Cube World, copy build/alpha-lookalike.dll to Mods, and optionally copy config/alpha-lookalike.ini. Restart. Works independently of MoonandShadow; update MoonandShadow too when migrating from its former integrated palette so grading is not doubled. NativePalette.ini is no longer used.

Settings: Enabled=1, SaturationPercent=90, CoolTintPercent=4 under [Palette] in Mods/alpha-lookalike.ini. ReShade is untouched; existing effects still influence the final image.

Targets inspected Steam executable timestamp 0x5d93ab0d/image size 0x5b7000. Native world draw constants identify the scene; grading occurs before the observed HUD boundary. Unknown boundaries skip grading rather than tinting menus. One GPU scene copy/resolve and one fullscreen pass; no CPU GPU readbacks. Actual target dimensions are used. WARP tests cover palette colors, state restoration and binding release at 720p, 1080p and 360p; live visual/UI/performance verification remains necessary.
