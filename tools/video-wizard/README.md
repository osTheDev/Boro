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
   For a batch of episodes press **Make them a TV show...**: type the show
   once, pick the season, keep the episode numbers from the file names (or
   number them in list order) and get the titles from TVmaze. It works on all
   rows, or just the ones you select.
   One file holding several episodes? Select it and press **Split into
   episodes...**: tick the chapters where episodes start (or let **Guess** do
   it from the episode length), or type start times if it has no chapters.
   Each part becomes its own episode. Files named only "Episode 3" take the
   folder's name as the show.
   - **Movie** -> `VIDEO\Movies\<Title>.mp4`
   - **TV episode** -> `VIDEO\Shows\<Show>\Season N\<Show> S01E03 <Title>.mp4`
   - **Music video** -> `VIDEO\Music\<Artist - Song>.mp4`
   - **Clip** -> `VIDEO\Clips\<Title>.mp4` (shows up in Boro's Photos)
3. **Poster.** For each video pick a frame from it (the arrows step further
   in), your own picture, or none. **Frames for the rest** skips ahead and
   gives every remaining video a frame from about 10% in. It's saved as `<name>.jpg` next to the video
   and Boro shows it on the video's tile. The first episode of a show that has
   no cover yet also asks for the show's cover (TVmaze, your own, or none),
   saved as the show's `cover.jpg`.
4. **Brew.** HandBrake converts them one at a time (H.264 Main, at most
   480x272, stereo AAC). **Pause** waits after the current one; **Stop** stops.
   When it's done it opens the PSP folder.

**Covers...** (on the first step) fixes posters for shows and movies you
already have: pick a folder (the VIDEO folder on the memory stick works), pick
one, flip through posters with < > (TVmaze for shows, iTunes for movies),
search by another name or use your own, then **Save cover**. Shows get
`cover.jpg` in their folder; movies get `<name>.jpg` beside the video.

Converted videos land in **<folder>\PSP\VIDEO**. Copy that VIDEO folder onto
the memory stick and let it merge. Originals move to **<folder>\Originals**. A
log is kept in `<folder>\PSP\BoroWizard.log`.

Posters on every tile need the Boro build that comes with this wizard; older
Boro shows them only on movie posters.
