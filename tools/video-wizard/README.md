# Boro Video Wizard (Windows)

An all-in-one tool for getting videos onto Boro: pick them, name them, give
them a poster, and it converts and files them the way Boro sorts them.

## Setup (once)

1. Get **HandBrakeCLI** (free): handbrake.fr > Downloads > *Command Line Version*.
   Unzip it and put `HandBrakeCLI.exe` in this folder (or install it anywhere on
   your PATH).
2. Optional: put **ffmpeg.exe** in this folder too. Without it, Windows reads the
   poster frames, which works for most MP4/MKV/MOV files; ffmpeg reads anything.
3. Double-click **Start-BoroWizard.bat**.

## The four steps

1. **Folder.** Browse to the folder with your videos (remembered next time in
   `BoroWizard.settings.txt`). Untick any you want to leave alone.
2. **What are they?** Each video gets a shelf and a name, guessed from the file
   name. Click a row to fix it. Red rows still need something.
   - **Movie** -> `VIDEO\Movies\<Title>.mp4`
   - **TV episode** -> `VIDEO\Shows\<Show>\Season N\<Show> S01E03 <Title>.mp4`
   - **Music video** -> `VIDEO\Music\<Artist - Song>.mp4`
   - **Clip** -> `VIDEO\Clips\<Title>.mp4` (shows up in Boro's Photos)
3. **Poster.** For each video pick a frame from it (the arrows step further
   in), your own picture, or none. It's saved as `<name>.jpg` next to the video
   and Boro shows it on the video's tile. The first episode of a show that has
   no cover yet also asks for the show's cover (TVmaze, your own, or none),
   saved as the show's `cover.jpg`.
4. **Brew.** HandBrake converts them one at a time (H.264 Main, at most
   480x272, stereo AAC). **Pause** waits after the current one; **Stop** stops.
   When it's done it opens the PSP folder.

Converted videos land in **<folder>\PSP\VIDEO**. Copy that VIDEO folder onto
the memory stick and let it merge. Originals move to **<folder>\Originals**. A
log is kept in `<folder>\PSP\BoroWizard.log`.

Posters on every tile need the Boro build that comes with this wizard; older
Boro shows them only on movie posters.
