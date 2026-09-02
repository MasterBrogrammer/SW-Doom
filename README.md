# SW-Doom

![SW-Doom](og/sw-doom-og-1280x640.jpg)

Mac SDL2 port of id Software's linuxdoom 1.10.

This repo is source plus the signed social image. It does not ship IWADs or a prebuilt `swdoom`. Commercial Doom data stays off GitHub. Freedoom is what you fetch yourself.

## Build

Needs clang, pkg-config, and SDL2. On macOS with Homebrew:

```
brew install sdl2
make -C linuxdoom-1.10
```

That writes `./swdoom`.

## IWAD

Put a Freedoom Phase 1 IWAD at `wads/freedoom1.wad`. Get it from [Freedoom](https://freedoom.github.io/) and copy `freedoom1.wad` into `wads/`.

Do not commit WAD files.

## Run

```
./swdoom -iwad wads/freedoom1.wad
```

## How to build the portable DMG

Needs a built `./swdoom`, `wads/freedoom1.wad` (Freedoom Phase 1), and Homebrew SDL2/SDL3 for the link and copy sources.

```
scripts/package-macos.sh
scripts/verify-macos-bundle.sh
```

`scripts/package-macos.sh` writes `dist/SW-Doom.app` and `dist/SW-Doom.dmg`. The app embeds rewritten SDL dylibs and Freedoom. The GitHub repo does not ship the IWAD. The local DMG does, because Freedoom is free.

## License

- Engine: GNU GPL v2. See `LICENSE.TXT` and `id-README.TXT`.
- Freedoom data, once you fetch it, uses the grant in `wads/COPYING.txt`.
