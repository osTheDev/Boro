# Boro

A music and video player for the PSP (built for a PSP-3000 on ARK-5).

## Install

1. Open the latest **Build EBOOT** run under the repository's Actions tab and download the `Boro-EBOOT` artifact.
2. Unzip it and copy the `PSP` folder to the root of your memory stick, so the file ends up at `PSP/GAME/Boro/EBOOT.PBP`.
3. Launch Boro from Game → Memory Stick.

## Controls (home)

| Button | Action |
| --- | --- |
| D-pad | Move between Music, Video and the stats bar |
| ✕ | Open |
| □ | Settings |
| ○ | Exit to the XMB |

## Music

Put MP3s anywhere under `MUSIC/` on the memory stick. Boro reads their ID3 tags and embedded album art, and caches the library in `PSP/GAME/Boro/data/`, so only new or changed files are read on later launches.

Artists in `MUSIC/RADIO/<Artist>/` are radio-only: they never show up in Albums, Artists or Songs.

| Button | FL/AC |
| --- | --- |
| D-pad | Move; left from the content goes to the sidebar |
| ✕ | Open / play |
| □ | Shuffle (album, artist, songs); sort albums |
| △ or SELECT | Now playing (on an artist page △ is radio) |
| L / R | Page through long lists; skip on Now playing |
| ○ | Back |

## Building locally

Install the [pspdev toolchain](https://github.com/pspdev/pspdev), then run `make`.

## Credits

Sora (The Sora Project Authors) and Manrope (The Manrope Project Authors) fonts, under the SIL Open Font License 1.1 (`res/fonts/OFL-*.txt`).
