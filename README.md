# Killer Bean Unleashed — Nintendo Switch port (Unity 2021.3.31f1 / IL2CPP wrapper)

This is a native wrapper / loader that runs the original ARM64 Android build of
Killer Bean Unleashed on Switch homebrew. It contains **no game code and no game
assets** — it loads the game's own libraries and recreates, natively, the thin
Android/JNI layer the Unity engine expects.

## Install & run

You need files from Killer Bean Unleashed 5.0.8 (arm64). Put the `.nro` in any folder
under `sdmc:/switch/` and place your game files next to it — the loader finds
its folder at runtime, so the name is up to you:

```
sdmc:/switch/killerbean
├── killerbean_nx.nro
├── libmain.so
├── libunity.so
├── libil2cpp.so
├── cursor.png                              <- optional
└── assets
```

Launch via title override (hold R while starting an installed game) or a
forwarder. Applet mode won't work; the loader needs the full memory of a game
override.

Optionally drop a `cursor.png` (up to 64×64, transparency respected) in the same
folder to replace the on-screen cursor with your own.

## Controls

The game is a touchscreen title: everything it understands is a touch landing on
one of the buttons it draws itself. The controller is mapped by injecting
touches at those positions, so a bound button and a finger are indistinguishable
to the game.

| Input | Action |
|---|---|
| Touchscreen | Direct multi-touch — the game as designed (handheld) |
| `+` | Toggle the on-screen cursor |
| ZL / ZR | Tap at the cursor while it's up |
| Left stick ←/→ | Move left / right |
| A | Jump |
| B | Fire |
| D-pad right | The green action button |
| L / R | Previous / next weapon |
| L + R | Recenter the cursor (while it's up) |
| `-` | Toggle gyro pointing |

## Building

Requires devkitPro with the `switch-dev` group plus these portlibs:

```
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 \
          switch-zlib switch-libpng
```

```
export DEVKITPRO=/opt/devkitpro
make                        # -> killerbean_nx.nro
```

Set `DEBUG_LOG 1` in `source/config.h` to get a `debug.log` next to the `.nro`.
Release builds have it off: every diagnostic in the tree goes dark, including
the crash dump and the JNI approximation ledger. Run `python3
tools/buildcheck.py source` after any edit — it catches things a Linux host
cannot, and the baseline is 2 issues.

## Credits

The loader/shim infrastructure (`so_util`, `libc_shim`, `jni_fake`,
`unity_jni`, `unity_input`, `android_native_unity`) derives from the
open-source Switch `.so`-loader lineage — Andy Nguyen and fgsfds, building on
TheOfficialFloW's Vita/Switch loader tradition — reaching this project by way of
the Zookeeper DX, PvZ Fusion and Fruit Ninja Classic ports. The controller →
on-screen-touch mapping and the cursor come from the Happy Wheels port. All
MIT-licensed.

Thanks to everyone in that lineage for making this approach possible.

## Legal

This repository contains only the wrapper. It ships no game code and no game
assets, and does not download, bundle or link to them. Running it requires files
from a copy of Killer Bean Unleashed that you own.

Killer Bean Unleashed is © Killer Bean Studios. This project is not affiliated
with or endorsed by Killer Bean Studios.
