# Boro

A music, video and games hub for the PSP (built for a PSP-3000 on ARK-5).

## Install

1. Open the latest **Build EBOOT** run under the repository's Actions tab and download the `Boro-EBOOT` artifact.
2. Unzip it and copy the `PSP` folder to the root of your memory stick, so the files end up at `PSP/GAME/Boro/EBOOT.PBP` and `PSP/GAME/Boro/boro_kernel.prx` (the small helper that lets Boro show the volume bar).
3. Launch Boro from Game → Memory Stick.

## Controls (home)

| Button | Action |
| --- | --- |
| D-pad | Move between Music, Video, Games and the stats bar |
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

## Games

Boro finds ISO and CSO files under `ISO/` (and its subfolders) and EBOOT games under `PSP/GAME/`, with their icons and titles. Nothing is moved or renamed.

- **Recent / All Games / Collections** tabs (L / R). ✕ opens a game's page, △ plays it, □ sorts All Games.
- **Game page:** background art, playtime, sessions, the last 7 days, ✕ Play, △ add to a collection.
- **Collections:** your own shelves, saved in `data/collections.txt`. Hold □ for 2 s to delete one, or to remove a game from one.
- **PSP menu order:** START on the Games screen. Pick games up with ✕ and move them; START saves by dating each ISO file or EBOOT folder, newest at the top.
- **Playtime** comes from the Boro Playtime plugin in [Boro-Plugins](https://github.com/osTheDev/Boro-Plugins) (`boro_time.prx`), which logs each session to `data/playtime.txt`. Game time also shows on the home stats bar and the Stats page.

## Building locally

Install the [pspdev toolchain](https://github.com/pspdev/pspdev), then run `make`.

## Credits

Sora (The Sora Project Authors), Manrope (The Manrope Project Authors), Archivo, Silkscreen, VT323 and Liberation Serif (Red Hat, Inc.) fonts, under the SIL Open Font License 1.1 (`res/fonts/OFL-*.txt`).
