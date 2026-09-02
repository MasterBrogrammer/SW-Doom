# SW-Doom

![SW-Doom](og/sw-doom-og-1280x640.jpg)

Mac SDL2 port of id Software's linuxdoom 1.10.

This repo is source, the signed social image, and the Mac disk image in `apple install`. It does not ship loose IWAD files. Commercial Doom data stays off GitHub.

## Mac install

The portable Mac disk image is [`apple install/SW-Doom.dmg`](apple%20install/SW-Doom.dmg).

Open that disk image to install or run the app.

Freedoom is already inside the `.app`. The Mac install does not need a separate IWAD.

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

## License

- Engine: GNU GPL v2. See `LICENSE.TXT` and `id-README.TXT`.
- Freedoom data, once you fetch it, uses the grant in `wads/COPYING.txt`.
