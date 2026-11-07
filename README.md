# Beyond Good & Evil – Enhanced Launcher

A launcher and add-on for the original PC release of **Beyond Good & Evil** (2003). It fixes the things that make the PC version awkward on a modern machine and adds modern post-processing, without changing any game data.

It's not a port or a decompilation. It works on your existing copy of the game.

> **Which version?** This is for the original 2003 game (`BGE.exe`), as sold on Ubisoft Connect, GOG and formerly Steam. It does **not** work with *Beyond Good & Evil – 20th Anniversary Edition* (2024), which is a separate remaster with its own engine and settings.

## Features

**Display fixes**
- **Locked 60 fps.** The Jade engine's physics and animation are tied to the frame rate, so a 120/144 Hz screen runs the game too fast. The built-in limiter holds a steady 60.
- **Widescreen (Hor+).** The game hard-codes a 4:3 picture and stretches it. This widens the view to your screen's real shape instead.
- **Field of view** slider, and an option to keep the **HUD and menus at 4:3** so they aren't stretched.
- **Any resolution**, fullscreen, windowed or **borderless**, with v-sync control.
- **Switch between borderless and windowed while playing** with a hotkey (Alt+Enter by default; F11, Ctrl+Enter or Alt+F on the Display tab). In windowed mode a lone Alt press no longer freezes the game by opening the window menu.
- **Anisotropic filtering** (up to 16x), and optional **MSAA** for the native renderer. MSAA is off by default because forcing it can cause black screens or broken graphics in parts of the game; use SMAA on the Vulkan renderer instead.

**Camera controls**
- Flip the **controller** or **mouse** camera **left/right and up/down separately**. The in-game Invert option only flips both together.
- Controller support comes from the game itself. The Ubisoft Connect version supports Xbox-style (XInput) controllers. Older GOG and Steam copies may have no controller support; use a mapper such as Steam Input or reWASD with those.

**Modern effects (ReShade on Vulkan)**
- **AMD FSR 1 RCAS** sharpening.
- **SMAA** anti-aliasing.
- **MXAO** ambient occlusion.
- **Screen-space global illumination** (NiceGuy Lighting).
- **Deband** for smoother sky gradients.

The depth-based effects (MXAO and GI) need the game's depth buffer, which ReShade can only read when the game runs through **DXVK** (Direct3D 9 → Vulkan). The launcher sets all of this up for you. Press **Home** in game for ReShade's menu.

**3D sound and EAX reverb**
- BG&E was made for sound cards with hardware 3D audio and **EAX 3.0** reverb. Modern Windows dropped both, so the game normally falls back to flat stereo.
- The launcher brings them back with [DSOAL](https://github.com/kcat/dsoal), which rebuilds DirectSound 3D and EAX in software on top of OpenAL Soft. You get positional sound and the game's own room reverb in caves, halls and other spaces.
- Choose the output on the Enhancements tab: Windows default, **headphones with 3D HRTF**, stereo speakers, 5.1 or 7.1.
- The game only turns EAX on if it can load Creative's `eax.dll`, which can't be redistributed, so the launcher ships a tiny open replacement (`src/eax`) that hands the call to DSOAL. It also sets the game's own "EAX reverb" registry options. Turning the option off puts everything back.

**Game settings**
- The game's own options (water, shadows, glow, blur, mipmapping and others) are on the Graphics tab, so you don't need the old settings app.

**Darkroom: get the 13th MDisk**
- The last MDisk is in a locked cabinet on the first floor of the Akuda Bar, next to Francis's puck table. Its code came from Ubisoft's Darkroom website, which has shut down.
- On the launcher's **Darkroom** tab, enter the 16-character Internet code from the game's Save/Load screen (shown in green). It gives you the cabinet code offline.
- The decoding method was reverse-engineered by the BG&E community. It's checked against the 100 real save-game codes in [daruma](https://github.com/9214/daruma)'s test data.

## Install

1. Download the latest release zip and extract it anywhere, for example `C:\Games\BGE-Enhanced`.
2. Run **BGELauncher.exe**. It finds the game automatically (Ubisoft Connect, Steam or GOG), or you can set the folder on the **Tools** tab.
3. Pick your settings and press **PLAY**.

The first time you enable SMAA, MXAO or GI, the launcher asks to download the iMMERSE shaders from their author's GitHub (see *Credits*).

**Requirements:** Windows 10 or 11, a GPU with Vulkan 1.3 support for the Vulkan renderer, and the .NET Framework 4.5 or later (built into Windows 10 and 11).

**Uninstall:** Tools tab → **Uninstall enhancements**. This removes every file the launcher added to the game folder and restores the original `BGE.exe`.

## What it changes on your system

- Copies its files into the game folder: `d3d9.dll`, `bgehook.ini`, ReShade, DXVK and the shaders.
- Sets the **large address aware** flag in `BGE.exe` so the 32-bit game can use 4 GB of memory, which DXVK needs. The original exe is backed up in the launcher's `backup` folder. Ubisoft Connect may undo this when it verifies the game; the launcher re-applies it every time you press Play.
- Registers ReShade as a Vulkan layer for your Windows user (`HKCU\SOFTWARE\Khronos\Vulkan\ImplicitLayers`). The layer only switches on inside BGE, through an environment variable the hook sets, so it doesn't affect other games.
- Writes the game's own video settings to its usual registry key.

## Status

Tested with the **Ubisoft Connect** version on Windows 11 with an NVIDIA RTX 4060: 60 fps lock, widescreen, borderless 1920×1080, and the full ReShade stack in gameplay. The Steam and GOG versions are detected but **untested**. If they behave differently, please open an issue.

Not included: ray tracing. RTX Remix was tried, but it doesn't work well enough with this game yet to ship.

## Building

- **Launcher:** run `build.bat`. It uses the C# compiler included with Windows, so there's nothing to install. Output: `build\BGELauncher.exe`.
- **Hook (`d3d9.dll`):** needs a 32-bit MinGW-w64 toolchain, for example [llvm-mingw](https://github.com/mstorsjo/llvm-mingw). Put it on PATH and run `sh src/bgehook/build.sh`. Output: `build\d3d9.dll` and `build\eax.dll`.
- **Release zip:** fill `third_party\` as listed below, then run `powershell -File package.ps1 -Version 1.1.0`.

`third_party\` (not committed):
```
ReShade32.dll                 from the ReShade 6.8.0 installer (reshade.me)
dxvk_d3d9.dll                 DXVK 3.1.1 x32\d3d9.dll, renamed
shaders\Shaders\ReShade.fxh   crosire/reshade-shaders (slim branch)
shaders\Shaders\Deband.fx     crosire/reshade-shaders (slim branch)
shaders\Shaders\NGLighting.fx, NGLighting-Configs.fxh, NGLighting-Shader.fxh, NGLightingUI.fxh
shaders\Textures\BlueNoise-64frames128x128.png    mj-ehsan/NiceGuy-Shaders
dsoal\dsound.dll, dsoal\dsoal-aldrv.dll           DSOAL_r697.zip (github.com/kcat/dsoal releases), DSOAL\Win32
```

## Credits

- [ReShade](https://reshade.me) by crosire. [DXVK](https://github.com/doitsujin/dxvk) by Philip Rebohle and contributors.
- [iMMERSE](https://github.com/martymcmodding/iMMERSE) (SMAA, MXAO, motion vectors) by Pascal "Marty McFly" Gilcher. It's downloaded from the author's own GitHub, not bundled.
- [NiceGuy Shaders](https://github.com/mj-ehsan/NiceGuy-Shaders) (NGLighting GI) by mj-ehsan.
- FSR 1 RCAS algorithm by AMD.

See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for licences. Beyond Good & Evil is © Ubisoft; this project is unofficial and includes no game files.

## Licence

MIT. See [LICENSE](LICENSE).
