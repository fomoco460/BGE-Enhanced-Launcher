# Third-party components

The release zip bundles these unmodified components. Their licence texts are in `licenses/`.

| Component | Version | Author | Licence | Bundled file(s) |
|---|---|---|---|---|
| [ReShade](https://reshade.me) | 6.8.0 | Patrick Mours (crosire) | BSD 3-Clause | `payload/ReShade32.dll` |
| [DXVK](https://github.com/doitsujin/dxvk) | 3.1.1 | Philip Rebohle and contributors | zlib | `payload/dxvk_d3d9.dll` (the 32-bit `d3d9.dll`, renamed) |
| [reshade-shaders](https://github.com/crosire/reshade-shaders) (slim) | — | crosire; Deband by Niklas Haas | CC0 (`ReShade.fxh`, SPDX header); MIT (`Deband.fx`, licence in the file header) | `ReShade.fxh`, `Deband.fx` |
| [NiceGuy Shaders](https://github.com/mj-ehsan/NiceGuy-Shaders) (NGLighting) | — | mj-ehsan (NiceGuy) | CC0 1.0 | `NGLighting*.fx/.fxh`, `BlueNoise-64frames128x128.png` |
| [DSOAL](https://github.com/kcat/dsoal) | r697 (commit e111282) | Chris Robinson (kcat) and contributors | LGPL 2.1 (includes fmt, MIT-style) | `payload/dsoal/dsound.dll` |
| [OpenAL Soft](https://github.com/kcat/openal-soft) | r10903 (commit 2fc9933), as built by DSOAL | Chris Robinson (kcat) and contributors | LGPL 2.0+ (parts Apache-2.0, BSD-3-Clause, PFFFT) | `payload/dsoal/dsoal-aldrv.dll` |

DSOAL and OpenAL Soft are the unmodified 32-bit binaries from DSOAL's official [`DSOAL_r697.zip`](https://github.com/kcat/dsoal/releases/tag/archive) release. Their source code is available at the links above. Under the LGPL you may replace these DLLs with your own builds.

`payload/dsoal/eax.dll` is not Creative's EAX library. It is a small open-source replacement written for this project (`src/eax`, MIT) that passes the game's `EAXDirectSoundCreate8` call to DSOAL.

`BGE_FSR_RCAS.fx` is this project's port of AMD FidelityFX Super Resolution 1 RCAS, an algorithm © Advanced Micro Devices, Inc., released under the MIT licence. `ReShadeUI.fxh` is a minimal stand-in written for this project.

## Not bundled: iMMERSE

SMAA, MXAO and the motion vectors used by the GI come from [iMMERSE](https://github.com/martymcmodding/iMMERSE) by Pascal "Marty McFly" Gilcher. Its licence forbids redistribution, so the launcher downloads it from the author's GitHub on your PC the first time you enable those effects, after asking you.

## The game

Beyond Good & Evil is © Ubisoft. This project contains no game files. The launcher sets the "large address aware" flag in your own copy of `BGE.exe` and keeps a backup of the original.
