<h1 align="center">He-Man: The Most Powerful Game in the Universe — PS Vita Port</h1>

<p align="center">
  <img src="docs/screenshots/title.jpg" width="49%">
  <img src="docs/screenshots/screenshot1.jpg" width="49%">
  <img src="docs/screenshots/screenshot2.jpg" width="49%">
  <img src="docs/screenshots/screenshot3.jpg" width="49%">
</p>

A wrapper/port of the Android version of *He-Man: The Most Powerful Game in the
Universe* for the PlayStation Vita. It loads the game's original ARMv7 native
library (`libnative-activity.so`) and runs it natively on the Vita, with the
Android APIs it needs reimplemented on top of the Vita's own (vitaGL for
OpenGL ES, a fake JVM/JNI, NDK shims, etc.).

This repository contains **no game files**. You need your own legally
obtained copy of the Android game.

**Español:** port de la versión Android de *He-Man: The Most Powerful Game in
the Universe* para PS Vita. No incluye archivos del juego: necesitas tu propia
copia. Las instrucciones de instalación están más abajo.

## Features

- Runs at a steady 60 fps on real hardware
- Physical controls: left stick / D-pad to move, face buttons mapped to the
  game's touch actions, **R** for "I Have The Power"; remappable in
  `ux0:data/heman/controls.txt`
- Fast loading: assets are served from RAM caches and an optional asset pack
  (`heman.pak`), level loads take about a third of the original time
- Fixes for crashes not seen on Android (music streaming race, buffer overflow
  in the engine's string buffers)

## Installation

1. Install [kubridge](https://github.com/bythos14/kubridge/releases) (add
   `ur0:tai/kubridge.skprx` under `*KERNEL` in `ur0:tai/config.txt`) and reboot.
2. Get the Android game (APK + OBB expansion file `main.obb`).
3. From the APK (it's a zip), copy `lib/armeabi-v7a/libnative-activity.so` to
   `ux0:data/heman/libnative-activity.so`.
4. Extract the contents of `main.obb` (also a zip) into `ux0:data/heman/`, so
   you end up with `ux0:data/heman/screen-large/`, `ux0:data/heman/sounds/`, …
5. Copy the APK's `assets/` folder to `ux0:data/heman/assets/`.
6. *(Recommended, much faster loading)* build the asset pack on your PC and
   copy it next to the other files:
   ```sh
   python3 tools/make_pak.py main.obb heman.pak
   # copy heman.pak to ux0:data/heman/heman.pak
   ```
   Keep the extracted files too: the pack only holds files up to 256 KB and the
   port falls back to the loose files for everything else. Rebuild the pack if
   you ever change `main.obb`.
7. Install `heman.vpk` with VitaShell.

### Controls

| Vita | Action (touch gesture it emulates) |
|---|---|
| Left stick / D-pad | Move |
| Cross | Swipe up (jump) |
| Square | Tap (attack) |
| Circle / L | Swipe right / left |
| Triangle | Top-right on-screen button |
| R | I Have The Power |
| Start | Top-left on-screen button (pause) |

The file `ux0:data/heman/controls.txt` is created on first launch and can be
edited (`<BUTTON> tap x y`, `<BUTTON> swipe x1 y1 x2 y2`, `<BUTTON> ihtp`,
screen is 960x544).

## Building

Requirements: [VitaSDK (softfp)](https://github.com/vitasdk-softfp) with the
`VITASDK` environment variable set.

```sh
mkdir build && cd build
cmake ..
make -j8
```

Useful CMake options:

| Option | Default | Description |
|---|---|---|
| `PERF_LOG` | OFF | Write frame timings and load stalls to `ux0:data/heman/perf.txt` |
| `HEAP_WATCH` | OFF | Heap corruption checks (slow, debugging only) |
| `MSAA_4X` | OFF | 4x MSAA (costs GPU time) |

The libraries in `lib/` (so_util, FalsoJNI, FalsoNDK, vitaGL) are included
directly instead of as submodules because they carry small fixes needed by
this port.

## Credits

This port is built on
**[soloader-boilerplate (SoLoBoP)](https://github.com/v-atamanenko/soloader-boilerplate)**
by **Volodymyr Atamanenko (gl33ntwine)**, the starter kit for Android-to-PS Vita
ports this project was created from. Thank you!

Also thanks to:

- **Andy Nguyen (TheFloW)** – original so_loader / so_util and FIOS helper
- **Rinnegatamante** – [so_util](https://github.com/Rinnegatamante/so_util) and [vitaGL](https://github.com/Rinnegatamante/vitaGL)
- **Volodymyr Atamanenko (gl33ntwine)** – [FalsoJNI](https://github.com/v-atamanenko/FalsoJNI)
- **elliencode** – [FalsoNDK](https://github.com/elliencode/FalsoNDK) and the [NBA Jam port](https://github.com/elliencode/nba-jam-psv), used as reference
- **bythos14** – [kubridge](https://github.com/bythos14/kubridge)
- The VitaSDK team and the PS Vita homebrew community

*He-Man* and *Masters of the Universe* are trademarks of Mattel. The game was
published by Chillingo. This project is not affiliated with or endorsed by
them.

## License

The port's code is released under the MIT license (see [LICENSE](LICENSE)),
like the boilerplate it is based on. Bundled libraries keep their own licenses
(vitaGL: LGPL-3.0, see `lib/vitagl/COPYING*`).
