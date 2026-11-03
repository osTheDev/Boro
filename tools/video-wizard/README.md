# Boro Video Wizard (Windows)

Turns videos you download into MP4s the PSP plays, and files them the way Boro
sorts them.

## Setup (once)

1. Get **HandBrakeCLI** (free): handbrake.fr > Downloads > *Command Line Version*.
   Unzip it and put `HandBrakeCLI.exe` in this folder (or install it anywhere on
   your PATH).
2. Double-click **Start-BoroWizard.bat**.

## Using it

- Leave it running. Any video that lands in **Downloads** (mp4, mkv, avi, mov,
  m4v, wmv, flv, webm, mpg, ts) is picked up once it has finished downloading.
- It converts one at a time to H.264 Main, at most 480x272, with stereo AAC.
- When one is done, the wizard asks what it is:
  - **Movie** -> `VIDEO\Movies\<Title>.mp4`
  - **TV episode** -> `VIDEO\Shows\<Show>\Season N\<Show> S01E03 <Title>.mp4`.
    For a new show it finds a cover on TVmaze (or pick your own) and saves it
    as `cover.jpg`, which Boro shows as the show's poster.
  - **Music video** -> `VIDEO\Music\<Artist - Song>.mp4`
  - **Clip** -> `VIDEO\Clips\<Title>.mp4` (shows up in Boro's Photos)
  
  It fills these in from the file name; if you don't answer in 2 minutes it
  files the video as shown.
- Converted videos collect in **Downloads\PSP\VIDEO**. Copy that VIDEO folder
  onto the memory stick and let it merge.
- Originals move to **Downloads\Originals**. A log is kept in
  `Downloads\PSP\BoroWizard.log`.
- **Pause** stops it starting the next video; **Quit** closes it.
