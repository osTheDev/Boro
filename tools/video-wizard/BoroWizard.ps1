# Boro Video Wizard
#
# Makes videos ready for Boro on the PSP, in four steps:
#
#   1. Pick a folder and tick the videos to work on.
#   2. Say what each one is (Movie, TV episode, Music video, Clip) and name it.
#   3. Pick a poster: a frame from the video, your own picture, or none.
#      TV shows also get a cover (TVmaze or your own).
#   4. HandBrakeCLI converts the batch to MP4s the PSP plays (H.264 Main, at
#      most 480x272, stereo AAC) and files them in <folder>\PSP\VIDEO:
#
#        VIDEO\Movies\<Title>.mp4            + <Title>.jpg
#        VIDEO\Shows\<Show>\cover.jpg
#        VIDEO\Shows\<Show>\Season N\<Show> S01E03 <Episode title>.mp4 + .jpg
#        VIDEO\Music\<Artist - Song>.mp4     + .jpg
#        VIDEO\Clips\<Title>.mp4             + .jpg
#
# Originals move to <folder>\Originals. Copy PSP\VIDEO onto the memory stick
# (merging folders). Start it with Start-BoroWizard.bat.

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# ---- Settings ---------------------------------------------------------------

$SettingsFile = Join-Path $PSScriptRoot 'BoroWizard.settings.txt'
$WorkDir   = Join-Path $env:TEMP 'BoroWizard'
$VideoExts = @('.mp4', '.mkv', '.avi', '.mov', '.m4v', '.wmv', '.flv', '.webm', '.mpg', '.mpeg', '.ts')
$FramePages = 4          # 5 frames a page, spread through the video

$Shelves = @(
    @{ Name = 'Movie';       Short = 'Movie'; Folder = 'Movies' },
    @{ Name = 'TV episode';  Short = 'TV';    Folder = 'Shows' },
    @{ Name = 'Music video'; Short = 'Music'; Folder = 'Music' },
    @{ Name = 'Clip';        Short = 'Clip';  Folder = 'Clips' }
)

function Find-Tool([string]$exe, [string[]]$more) {
    $here = Join-Path $PSScriptRoot $exe
    if (Test-Path -LiteralPath $here) { return $here }
    $cmd = Get-Command $exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($p in $more) { if ($p -and (Test-Path -LiteralPath $p)) { return $p } }
    return $null
}

$HandBrake = Find-Tool 'HandBrakeCLI.exe' @("$env:ProgramFiles\HandBrake\HandBrakeCLI.exe",
    "${env:ProgramFiles(x86)}\HandBrake\HandBrakeCLI.exe", "$env:ProgramFiles\HandBrakeCLI\HandBrakeCLI.exe")
if (-not $HandBrake) {
    [Windows.Forms.MessageBox]::Show(
        "HandBrakeCLI.exe wasn't found.`n`nDownload the command line version from handbrake.fr (Downloads > Command Line Version), " +
        "then put HandBrakeCLI.exe in this folder:`n`n$PSScriptRoot",
        'Boro Video Wizard', 'OK', 'Warning') | Out-Null
    exit 1
}
# Optional: ffmpeg.exe reads frames from any video. Without it, Windows does.
$FFmpeg = Find-Tool 'ffmpeg.exe' @()

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

$Folder = ''
if (Test-Path -LiteralPath $SettingsFile) {
    $line = Get-Content -LiteralPath $SettingsFile | Where-Object { $_ -like 'Folder=*' } | Select-Object -First 1
    if ($line -and (Test-Path -LiteralPath $line.Substring(7))) { $Folder = $line.Substring(7) }
}

function Set-Folder([string]$dir) {
    $script:Folder    = $dir
    $script:OutRoot   = Join-Path $dir 'PSP\VIDEO'
    $script:Originals = Join-Path $dir 'Originals'
    $script:LogFile   = Join-Path $dir 'PSP\BoroWizard.log'
}
if ($Folder) { Set-Folder $Folder }

function Write-Log([string]$msg) {
    if (-not $script:LogFile) { return }
    New-Item -ItemType Directory -Force -Path (Split-Path $script:LogFile) -ErrorAction SilentlyContinue | Out-Null
    $line = '{0:yyyy-MM-dd HH:mm:ss}  {1}' -f (Get-Date), $msg
    Add-Content -LiteralPath $script:LogFile -Value $line -ErrorAction SilentlyContinue
}

# ---- Names ------------------------------------------------------------------

$Junk = '(?i)[\s._\-\[\(]+(2160p|1080p|720p|576p|480p|4k|uhd|x264|x265|h\.?264|h\.?265|hevc|web[\s._-]?dl|webrip|bluray|blu-ray|brrip|bdrip|hdtv|hdrip|dvdrip|xvid|aac\d?(\.\d)?|ac3|dts|ddp?5\.1|proper|repack|extended|remastered|10bit|hdr)\b.*$'

function Clean-Name([string]$s) {
    $s = $s -replace '[._]', ' '
    $s = $s -replace $Junk, ''
    $s = $s -replace '\[[^\]]*\]', ''
    $s = $s -replace '\s{2,}', ' '
    return $s.Trim(' ', '-')
}

# Characters Windows or the memory stick won't take in a name.
function Safe-Name([string]$s) {
    $s = $s -replace '[\\/:*?"<>|\[\]]', '' -replace '\s{2,}', ' '
    return $s.Trim(' ', '.')
}

# A new item for a video, with a guess at what it is from its name.
function New-Item-For([string]$path) {
    $base = [IO.Path]::GetFileNameWithoutExtension($path)
    $it = @{ Src = $path; Shelf = 0; Title = ''; Show = ''; Season = 1; Episode = 1
             Poster = 'frame'; FramePage = 0; FrameSel = 1; Frames = @{}; Times = @{}; Own = $null
             Duration = -1.0; Clip = $null; NoFrames = $false; Status = ''; EpGuessed = $false }
    if ($base -match '^(.*?)[\s._\-]*[Ss](\d{1,2})[\s._]?[Ee](\d{1,3})(.*)$' -or
        $base -match '^(.*?)[\s._\-]+(\d{1,2})x(\d{2,3})(.*)$') {
        $it.Shelf = 1
        $it.Show = Clean-Name ($Matches[1] -replace '[\s._\-]*\(?(19|20)\d{2}\)?$', '')
        $it.Season = [Math]::Max(1, [Math]::Min(99, [int]$Matches[2]))
        $it.Episode = [Math]::Max(1, [Math]::Min(999, [int]$Matches[3]))
        $it.Title = Clean-Name (" " + $Matches[4])
        $it.EpGuessed = $true
        $known = Existing-Shows | Where-Object { $_ -ieq $it.Show } | Select-Object -First 1
        if ($known) { $it.Show = $known }
        return $it
    }
    # "Show - Episode 3 - Title", "Show Ep 03", "Show E03"
    if ($base -match '^(.+?)[\s._\-]+(?:episode|ep|e)[\s._]*(\d{1,3})\b[\s._\-]*(.*)$') {
        $it.Shelf = 1
        $it.Show = Clean-Name $Matches[1]
        $it.Episode = [Math]::Max(1, [int]$Matches[2])
        $it.Title = Clean-Name (" " + $Matches[3])
        $it.EpGuessed = $true
        $known = Existing-Shows | Where-Object { $_ -ieq $it.Show } | Select-Object -First 1
        if ($known) { $it.Show = $known }
        return $it
    }
    $t = Clean-Name $base
    $t = ($t -replace '\s*\(?\b(19|20)\d{2}\b\)?\s*$', '').Trim()
    if (-not $t) { $t = Clean-Name $base }
    if ($base -match ' - ' -and $base -notmatch '(?i)\b(19|20)\d{2}\b') { $it.Shelf = 2 }
    $it.Title = $t
    return $it
}

function Existing-Shows {
    if (-not $script:OutRoot) { return @() }
    $dir = Join-Path $script:OutRoot 'Shows'
    if (-not (Test-Path -LiteralPath $dir)) { return @() }
    return @(Get-ChildItem -LiteralPath $dir -Directory | Sort-Object Name | ForEach-Object { $_.Name })
}

# What's still missing before an item can be filed, or ''.
function Missing($it) {
    if ($it.Shelf -eq 1) { if (-not (Safe-Name $it.Show)) { return 'show' }; return '' }
    if (-not (Safe-Name $it.Title)) { return 'title' }
    return ''
}

# How Boro will list it.
function Boro-Name($it) {
    if ($it.Shelf -eq 1) {
        $s = 'S{0:D2}E{1:D2}' -f [int]$it.Season, [int]$it.Episode
        if ($it.Title.Trim()) { $s += ' ' + $it.Title.Trim() }
        if (-not $it.Show.Trim()) { $s += '  ?' }
        return $s
    }
    if (-not $it.Title.Trim()) { return '?' }
    return $it.Title.Trim()
}

function Target-Path($it) {
    $folder = Join-Path $script:OutRoot $Shelves[$it.Shelf].Folder
    $title = Safe-Name $it.Title
    if ($it.Shelf -eq 1) {
        $show = Safe-Name $it.Show
        $file = '{0} S{1:D2}E{2:D2}' -f $show, [int]$it.Season, [int]$it.Episode
        if ($title) { $file += " $title" }
        return Join-Path (Join-Path (Join-Path $folder $show) ("Season {0}" -f [int]$it.Season)) "$file.mp4"
    }
    return Join-Path $folder "$title.mp4"
}

function Unique-Path([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $path }
    $dir = Split-Path $path; $stem = [IO.Path]::GetFileNameWithoutExtension($path); $ext = [IO.Path]::GetExtension($path)
    for ($i = 2; ; $i++) {
        $p = Join-Path $dir ("{0} ({1}){2}" -f $stem, $i, $ext)
        if (-not (Test-Path -LiteralPath $p)) { return $p }
    }
}

# ---- Pictures -----------------------------------------------------------------

function Load-Image([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $ms = New-Object IO.MemoryStream(, $bytes)
    return [Drawing.Image]::FromStream($ms)
}

# Saves a picture as a JPEG no bigger than $max on its long side.
function Save-Jpeg([Drawing.Image]$img, [string]$path, [int]$max) {
    $k = [Math]::Min(1.0, $max / [double][Math]::Max($img.Width, $img.Height))
    $w = [Math]::Max(1, [int]($img.Width * $k)); $h = [Math]::Max(1, [int]($img.Height * $k))
    $bmp = New-Object Drawing.Bitmap($w, $h)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.DrawImage($img, 0, 0, $w, $h)
    $g.Dispose()
    $bmp.Save($path, [Drawing.Imaging.ImageFormat]::Jpeg)
    $bmp.Dispose()
}

# TVmaze's poster for a show, as a picture, or $null.
function Find-Cover([string]$show) {
    try {
        $q = [uri]::EscapeDataString($show)
        $r = Invoke-RestMethod -Uri "https://api.tvmaze.com/singlesearch/shows?q=$q" -TimeoutSec 8
        $url = $r.image.original
        if (-not $url) { $url = $r.image.medium }
        if (-not $url) { return $null }
        $tmp = Join-Path $WorkDir 'cover-download'
        Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing -TimeoutSec 15
        return Load-Image $tmp
    } catch {
        Write-Log "No cover from TVmaze for '$show': $($_.Exception.Message)"
        return $null
    }
}

function Has-Cover([string]$show) {
    $dir = Join-Path (Join-Path $script:OutRoot 'Shows') (Safe-Name $show)
    foreach ($n in 'cover', 'poster', 'folder') {
        foreach ($e in '.jpg', '.jpeg', '.png') { if (Test-Path -LiteralPath (Join-Path $dir "$n$e")) { return $true } }
    }
    return $false
}

# ---- Frames from a video ---------------------------------------------------------

# Runs a program with no window and returns what it wrote to stderr.
function Run-Hidden([string]$exe, [string]$arguments, [int]$ms = 20000) {
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $exe; $psi.Arguments = $arguments
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($psi)
    $err = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($ms)) { try { $p.Kill() } catch { } }
    return $err.Result
}

$WinRT = $null
function Init-WinRT {
    if ($null -ne $script:WinRT) { return $script:WinRT }
    try {
        Add-Type -AssemblyName System.Runtime.WindowsRuntime
        $script:AsTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
            $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } |
            Select-Object -First 1
        $null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
        $null = [Windows.Media.Editing.MediaClip, Windows.Media, ContentType = WindowsRuntime]
        $null = [Windows.Media.Editing.MediaComposition, Windows.Media, ContentType = WindowsRuntime]
        $null = [Windows.Graphics.Imaging.ImageStream, Windows.Graphics, ContentType = WindowsRuntime]
        $script:WinRT = $true
    } catch {
        Write-Log "Windows video frames unavailable: $($_.Exception.Message)"
        $script:WinRT = $false
    }
    return $script:WinRT
}
function Await($op, [Type]$type) {
    $t = $script:AsTask.MakeGenericMethod($type).Invoke($null, @($op))
    if (-not $t.Wait(20000)) { throw 'Windows took too long' }
    return $t.Result
}

# Seconds of video, or -1 if it can't be read. Also opens it for Get-Frame.
function Get-Duration($it) {
    if ($it.Duration -ge 0) { return $it.Duration }
    $it.Duration = 0
    try {
        if ($FFmpeg) {
            $out = Run-Hidden $FFmpeg ('-hide_banner -i "{0}"' -f $it.Src)
            if ($out -match 'Duration:\s*(\d+):(\d+):(\d+(\.\d+)?)') {
                $it.Duration = [int]$Matches[1] * 3600 + [int]$Matches[2] * 60 + [double]$Matches[3]
            }
        } elseif (Init-WinRT) {
            $file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($it.Src)) ([Windows.Storage.StorageFile])
            $clip = Await ([Windows.Media.Editing.MediaClip]::CreateFromFileAsync($file)) ([Windows.Media.Editing.MediaClip])
            $comp = New-Object Windows.Media.Editing.MediaComposition
            $comp.Clips.Add($clip)
            $it.Clip = @{ Comp = $comp; W = [int]$clip.GetVideoEncodingProperties().Width; H = [int]$clip.GetVideoEncodingProperties().Height }
            $it.Duration = $clip.OriginalDuration.TotalSeconds
        }
    } catch { Write-Log "Couldn't read $($it.Src): $($_.Exception.Message)" }
    if ($it.Duration -le 0) { $it.Duration = -1; $it.NoFrames = $true }
    return $it.Duration
}

# One frame at $sec, about 480 wide, or $null.
function Get-Frame($it, [double]$sec) {
    try {
        if ($FFmpeg) {
            $out = Join-Path $WorkDir ('frame-{0}.jpg' -f [guid]::NewGuid().ToString('N').Substring(0, 8))
            Run-Hidden $FFmpeg ('-hide_banner -loglevel error -ss {0} -i "{1}" -frames:v 1 -vf scale=480:-2 -y "{2}"' -f
                $sec.ToString([Globalization.CultureInfo]::InvariantCulture), $it.Src, $out) | Out-Null
            if (-not (Test-Path -LiteralPath $out)) { return $null }
            $img = Load-Image $out
            Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
            return $img
        }
        if ($it.Clip) {
            $w = 480; $h = 270
            if ($it.Clip.W -gt 0 -and $it.Clip.H -gt 0) { $h = [int](480 * $it.Clip.H / $it.Clip.W) }
            $s = Await ($it.Clip.Comp.GetThumbnailAsync([TimeSpan]::FromSeconds($sec), $w, $h,
                [Windows.Media.Editing.VideoFramePrecision]::NearestKeyFrame)) ([Windows.Graphics.Imaging.ImageStream])
            $rs = [IO.WindowsRuntimeStreamExtensions]::AsStreamForRead($s)
            $ms = New-Object IO.MemoryStream
            $rs.CopyTo($ms); $rs.Dispose()
            $ms.Position = 0
            return [Drawing.Image]::FromStream($ms)
        }
    } catch { Write-Log "No frame at $sec s from $($it.Src): $($_.Exception.Message)" }
    return $null
}

# The five frames of one page, grabbed the first time they're asked for.
function Get-Frames($it, [int]$page) {
    if ($it.Frames.ContainsKey($page)) { return }
    $d = Get-Duration $it
    $imgs = @(); $times = @()
    for ($i = 0; $i -lt 5; $i++) {
        $sec = 0.0
        if ($d -gt 0) { $sec = $d * ($page * 5 + $i + 1) / ($FramePages * 5 + 1) }
        $times += $sec
        if ($d -gt 0) { $imgs += , (Get-Frame $it $sec) } else { $imgs += , $null }
    }
    $it.Frames[$page] = $imgs; $it.Times[$page] = $times
    if (-not ($imgs | Where-Object { $_ })) { $it.NoFrames = $true }
}

function Format-Time([double]$s) {
    $t = [TimeSpan]::FromSeconds([Math]::Floor($s))
    if ($t.TotalHours -ge 1) { return '{0}:{1:D2}:{2:D2}' -f [int][Math]::Floor($t.TotalHours), $t.Minutes, $t.Seconds }
    return '{0}:{1:D2}' -f $t.Minutes, $t.Seconds
}

# ---- Window -----------------------------------------------------------------------

$Gray = [Drawing.Color]::FromArgb(192, 192, 192)
$Navy = [Drawing.Color]::FromArgb(0, 0, 128)
$Dim  = [Drawing.Color]::FromArgb(60, 60, 60)
$Red  = [Drawing.Color]::FromArgb(170, 0, 0)
$Font = New-Object Drawing.Font('Microsoft Sans Serif', 8.25)
$Bold = New-Object Drawing.Font('Microsoft Sans Serif', 8.25, [Drawing.FontStyle]::Bold)
$Head = New-Object Drawing.Font('Microsoft Sans Serif', 12, [Drawing.FontStyle]::Bold)

function New-Label($text, $x, $y, $w = 360, $font = $Font) {
    $l = New-Object Windows.Forms.Label
    $l.Text = $text; $l.Location = New-Object Drawing.Point($x, $y); $l.Size = New-Object Drawing.Size($w, $(if ($font -eq $Head) { 22 } else { 16 })); $l.Font = $font
    return $l
}
function New-Button($text, $x, $y, $w = 75, $h = 23) {
    $b = New-Object Windows.Forms.Button
    $b.Text = $text; $b.Location = New-Object Drawing.Point($x, $y); $b.Size = New-Object Drawing.Size($w, $h)
    $b.FlatStyle = 'Standard'; $b.BackColor = $Gray; $b.Font = $Font
    return $b
}
function New-Page {
    $p = New-Object Windows.Forms.Panel
    $p.Location = New-Object Drawing.Point(146, 8); $p.Size = New-Object Drawing.Size(366, 306); $p.Visible = $false
    $Form.Controls.Add($p)
    return $p
}

$Form = New-Object Windows.Forms.Form
$Form.Text = 'Boro Video Wizard'
$Form.ClientSize = New-Object Drawing.Size(520, 360)
$Form.FormBorderStyle = 'FixedSingle'; $Form.MaximizeBox = $false
$Form.BackColor = $Gray; $Form.Font = $Font
$Form.StartPosition = 'CenterScreen'

# The sidebar: a starry night and a little wizard of our own.
$Wizard = @('......YY......', '.....BBBY.....', '....BBYBB.....', '...BBBBBBB....', '..BBBBBYBBB...', 'BBBBBBBBBBBBBB',
            '.....SSSS.....', '.....EsSE.....', '....WSSSSW....', '...WWWWWWWW...', '..PWWWWWWWWP..', '.PPPWWWWWWPPP.',
            '.PPPPWWWWPPPPS', 'PPPPPPPPPPPPP#', 'PPPPPPPPPPPP.#', 'PPPPPPPPPPPP.#', '.PPPPPPPPPP..#', '.PPPPPPPPPP..#', '..BB....BB...#')
# Case matters ('S' skin, 's' shade), so not a PowerShell hashtable.
$Colors = New-Object 'System.Collections.Generic.Dictionary[string,System.Drawing.Color]'
$Colors['Y'] = [Drawing.Color]::FromArgb(255, 220, 60); $Colors['B'] = [Drawing.Color]::FromArgb(60, 40, 160)
$Colors['S'] = [Drawing.Color]::FromArgb(240, 200, 160); $Colors['E'] = [Drawing.Color]::Black
$Colors['s'] = [Drawing.Color]::FromArgb(220, 170, 130); $Colors['W'] = [Drawing.Color]::FromArgb(235, 235, 235)
$Colors['P'] = [Drawing.Color]::FromArgb(90, 60, 190); $Colors['#'] = [Drawing.Color]::FromArgb(140, 90, 40)
$Side = New-Object Windows.Forms.Panel
$Side.Location = New-Object Drawing.Point(8, 8); $Side.Size = New-Object Drawing.Size(130, 304); $Side.BackColor = $Navy
$Side.Add_Paint({
    param($s, $e)
    $g = $e.Graphics
    $star = New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(255, 255, 180))
    for ($i = 0; $i -lt 300; $i += 6) { $g.FillRectangle($star, 4 + ($i * 7) % 122, 4 + $i, 1, 1) }
    $px = 6
    for ($r = 0; $r -lt $Wizard.Count; $r++) {
        for ($c = 0; $c -lt $Wizard[$r].Length; $c++) {
            $ch = [string]$Wizard[$r][$c]
            if ($Colors.ContainsKey($ch)) {
                $b = New-Object Drawing.SolidBrush($Colors[$ch])
                $g.FillRectangle($b, 23 + $c * $px, 60 + $r * $px, $px, $px)
                $b.Dispose()
            }
        }
    }
    $big = New-Object Drawing.Font('Microsoft Sans Serif', 13, [Drawing.FontStyle]::Bold)
    $g.DrawString('Boro Video', $big, [Drawing.Brushes]::White, 10, 196)
    $g.DrawString('Wizard', $big, (New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(255, 220, 60))), 10, 218)
})
$Form.Controls.Add($Side)

$Rule = New-Object Windows.Forms.Label
$Rule.BorderStyle = 'Fixed3D'; $Rule.Location = New-Object Drawing.Point(8, 320); $Rule.Size = New-Object Drawing.Size(504, 2)
$Form.Controls.Add($Rule)
$StepLabel = New-Label '' 12 334 100; $StepLabel.ForeColor = $Dim; $Form.Controls.Add($StepLabel)
$BackBtn = New-Button '< Back' 275 330; $Form.Controls.Add($BackBtn)
$NextBtn = New-Button 'Next >' 356 330; $Form.Controls.Add($NextBtn)
$CancelBtn = New-Button 'Cancel' 437 330; $Form.Controls.Add($CancelBtn)

$All   = @{}                                        # path -> item, so edits survive going back
$Items = New-Object System.Collections.ArrayList    # the ticked videos, in order
$Step  = 1
$Loading = $false                                   # true while filling controls from an item

# ---- Step 1: folder ----
$P1 = New-Page
$P1.Controls.Add((New-Label 'Which videos are we brewing?' 0 4 366 $Head))
$P1.Controls.Add((New-Label "Pick a folder. I'll only touch the videos inside it." 0 30))
$P1.Controls.Add((New-Label 'Folder:' 0 52 366 $Bold))
$FolderBox = New-Object Windows.Forms.TextBox
$FolderBox.Location = New-Object Drawing.Point(0, 68); $FolderBox.Size = New-Object Drawing.Size(280, 21); $FolderBox.ReadOnly = $true
$FolderBox.BackColor = [Drawing.Color]::White
$P1.Controls.Add($FolderBox)
$BrowseBtn = New-Button 'Browse...' 286 67; $P1.Controls.Add($BrowseBtn)
$FoundLabel = New-Label '' 0 98 366 $Bold; $P1.Controls.Add($FoundLabel)
$FileList = New-Object Windows.Forms.CheckedListBox
$FileList.Location = New-Object Drawing.Point(0, 116); $FileList.Size = New-Object Drawing.Size(360, 100)
$FileList.CheckOnClick = $true; $FileList.IntegralHeight = $false
$P1.Controls.Add($FileList)
$Untick = New-Label 'Untick any you want to leave alone.' 0 220; $Untick.ForeColor = $Dim; $P1.Controls.Add($Untick)
$OutLabel = New-Label '' 0 242 366; $OutLabel.Size = New-Object Drawing.Size(366, 32); $P1.Controls.Add($OutLabel)
$Remember = New-Label "I'll remember this folder for next time." 0 280; $Remember.ForeColor = $Dim; $P1.Controls.Add($Remember)
$Files = @()     # full paths, in list order

function Fill-Files {
    $FileList.Items.Clear(); $script:Files = @()
    if (-not $script:Folder) {
        $FolderBox.Text = ''; $FoundLabel.Text = 'Pick a folder to start.'; $OutLabel.Text = ''
        return
    }
    $FolderBox.Text = $script:Folder
    $vids = @(Get-ChildItem -LiteralPath $script:Folder -File -ErrorAction SilentlyContinue |
        Where-Object { $VideoExts -contains $_.Extension.ToLower() } | Sort-Object Name)
    foreach ($v in $vids) {
        $size = if ($v.Length -ge 1GB) { '{0:0.0} GB' -f ($v.Length / 1GB) } else { '{0:0} MB' -f [Math]::Max(1, $v.Length / 1MB) }
        [void]$FileList.Items.Add(("{0}    ({1})" -f $v.Name, $size), $true)
        $script:Files += $v.FullName
    }
    if ($vids.Count -eq 0) { $FoundLabel.Text = 'No videos in this folder.' }
    elseif ($vids.Count -eq 1) { $FoundLabel.Text = 'I found 1 video:' }
    else { $FoundLabel.Text = "I found $($vids.Count) videos:" }
    $name = Split-Path $script:Folder -Leaf
    $OutLabel.Text = "PSP-ready videos go to  ...\$name\PSP\VIDEO`nOriginals go to  ...\$name\Originals"
}

$BrowseBtn.Add_Click({
    $dlg = New-Object Windows.Forms.FolderBrowserDialog
    $dlg.Description = 'Pick the folder with the videos for your PSP.'
    if ($script:Folder) { $dlg.SelectedPath = $script:Folder }
    if ($dlg.ShowDialog() -ne 'OK') { return }
    Set-Folder $dlg.SelectedPath
    Set-Content -LiteralPath $SettingsFile -Value ("Folder=" + $dlg.SelectedPath) -ErrorAction SilentlyContinue
    Fill-Files
    Update-Nav
})
# ItemCheck fires before the tick changes, so count the one changing.
$FileList.Add_ItemCheck({
    param($s, $e)
    $n = $FileList.CheckedIndices.Count
    if ($e.NewValue -eq 'Checked') { $n++ } else { $n-- }
    if ($script:Step -eq 1) { $NextBtn.Enabled = $n -gt 0 }
})

# ---- Step 2: names ----
$P2 = New-Page
$P2.Controls.Add((New-Label 'What are they?' 0 4 366 $Head))
$Table = New-Object Windows.Forms.ListView
$Table.Location = New-Object Drawing.Point(0, 28); $Table.Size = New-Object Drawing.Size(360, 92)
$Table.View = 'Details'; $Table.FullRowSelect = $true; $Table.HideSelection = $false; $Table.MultiSelect = $true
$Table.HeaderStyle = 'Nonclickable'
[void]$Table.Columns.Add('File', 130); [void]$Table.Columns.Add('Shelf', 46); [void]$Table.Columns.Add('Name in Boro', 162)
$P2.Controls.Add($Table)
$P2.Controls.Add((New-Label 'Shelf:' 0 126 366 $Bold))
$Radios = @()
$rx = @(0, 62, 152, 252)
for ($i = 0; $i -lt 4; $i++) {
    $r = New-Object Windows.Forms.RadioButton
    $r.Text = $Shelves[$i].Name; $r.Location = New-Object Drawing.Point($rx[$i], 141); $r.AutoSize = $true; $r.Tag = $i
    $P2.Controls.Add($r); $Radios += $r
}
$ShowLabel = New-Label 'Show:' 0 166 200 $Bold; $P2.Controls.Add($ShowLabel)
$ShowBox = New-Object Windows.Forms.ComboBox
$ShowBox.Location = New-Object Drawing.Point(0, 182); $ShowBox.Size = New-Object Drawing.Size(196, 21); $ShowBox.DropDownStyle = 'DropDown'
$P2.Controls.Add($ShowBox)
$SeasonLabel = New-Label 'Season:' 206 166 70 $Bold; $P2.Controls.Add($SeasonLabel)
$SeasonBox = New-Object Windows.Forms.NumericUpDown
$SeasonBox.Location = New-Object Drawing.Point(206, 182); $SeasonBox.Size = New-Object Drawing.Size(60, 21); $SeasonBox.Minimum = 1; $SeasonBox.Maximum = 99
$P2.Controls.Add($SeasonBox)
$EpLabel = New-Label 'Episode:' 276 166 80 $Bold; $P2.Controls.Add($EpLabel)
$EpBox = New-Object Windows.Forms.NumericUpDown
$EpBox.Location = New-Object Drawing.Point(276, 182); $EpBox.Size = New-Object Drawing.Size(84, 21); $EpBox.Minimum = 1; $EpBox.Maximum = 999
$P2.Controls.Add($EpBox)
$TitleLabel = New-Label 'Title:' 0 208 366 $Bold; $P2.Controls.Add($TitleLabel)
$TitleBox = New-Object Windows.Forms.TextBox
$TitleBox.Location = New-Object Drawing.Point(0, 224); $TitleBox.Size = New-Object Drawing.Size(360, 21)
$P2.Controls.Add($TitleBox)
$ShowsAs = New-Label '' 0 252 366; $P2.Controls.Add($ShowsAs)
$Hint = New-Label 'I guessed from the file names. Click a row to fix it (Ctrl/Shift for several).' 0 272; $Hint.ForeColor = $Dim; $P2.Controls.Add($Hint)
$RedNote = New-Label 'Red = still needs something.' 0 288; $RedNote.ForeColor = $Red; $P2.Controls.Add($RedNote)

function Selected-Item { if ($Table.SelectedItems.Count) { return $Table.SelectedItems[0].Tag }; return $null }
function Selected-Rows { return @($Table.SelectedItems | ForEach-Object { $_ }) }

function Update-Row($row) {
    $it = $row.Tag
    $row.SubItems[1].Text = $Shelves[$it.Shelf].Short
    $row.SubItems[2].Text = Boro-Name $it
    if (Missing $it) { $row.ForeColor = $Red } else { $row.ForeColor = [Drawing.Color]::Black }
}

function Fill-Table {
    $Table.BeginUpdate(); $Table.Items.Clear()
    foreach ($it in $Items) {
        $row = New-Object Windows.Forms.ListViewItem((Split-Path $it.Src -Leaf))
        [void]$row.SubItems.Add(''); [void]$row.SubItems.Add('')
        $row.Tag = $it
        Update-Row $row
        [void]$Table.Items.Add($row)
    }
    $Table.EndUpdate()
    $ShowBox.Items.Clear()
    $names = @(Existing-Shows) + @($Items | Where-Object { $_.Shelf -eq 1 -and $_.Show.Trim() } | ForEach-Object { $_.Show.Trim() })
    foreach ($s in ($names | Sort-Object -Unique)) { [void]$ShowBox.Items.Add($s) }
    if ($Table.Items.Count) { $Table.Items[0].Selected = $true }
}

function Load-Editor {
    $it = Selected-Item
    $script:Loading = $true
    foreach ($c in @($Radios + @($ShowBox, $SeasonBox, $EpBox, $TitleBox))) { $c.Enabled = [bool]$it }
    if ($it) {
        $Radios[$it.Shelf].Checked = $true
        $ShowBox.Text = $it.Show; $SeasonBox.Value = $it.Season; $EpBox.Value = $it.Episode; $TitleBox.Text = $it.Title
        # Several rows: shelf, show and season go to all of them; titles one at a time.
        $one = $Table.SelectedItems.Count -eq 1
        $EpBox.Enabled = $one; $TitleBox.Enabled = $one
        if (-not $one) { $TitleBox.Text = '' }
    }
    $script:Loading = $false
    Update-Editor
}

function Update-Editor {
    $it = Selected-Item
    $tv = $it -and $it.Shelf -eq 1
    foreach ($c in @($ShowLabel, $ShowBox, $SeasonLabel, $SeasonBox, $EpLabel, $EpBox)) { $c.Visible = $tv }
    if ($tv) { $TitleLabel.Text = 'Episode title (optional):' } elseif ($it -and $it.Shelf -eq 2) { $TitleLabel.Text = 'Artist - Song:' } else { $TitleLabel.Text = 'Title:' }
    if (-not $it) { $ShowsAs.Text = ''; return }
    if ($Table.SelectedItems.Count -gt 1) {
        $ShowsAs.Text = "Editing $($Table.SelectedItems.Count) videos: shelf, show and season apply to all."
        return
    }
    if ($tv) {
        $ep = "$($it.Episode)."; if ($it.Title.Trim()) { $ep += ' ' + $it.Title.Trim() }
        $show = $it.Show.Trim(); if (-not $show) { $show = '(show?)' }
        $ShowsAs.Text = "Boro shows it as:   $show  >  Season $($it.Season)  >  $ep"
    } else {
        $ShowsAs.Text = "Boro shows it as:   $(Boro-Name $it)   (in $($Shelves[$it.Shelf].Folder))"
    }
}

# Copies one field of the editor into the selected item(s).
function Save-Editor([string]$field) {
    if ($script:Loading) { return }
    $rows = Selected-Rows
    if (-not $rows.Count) { return }
    $n = 0
    foreach ($row in $rows) {
        $it = $row.Tag
        switch ($field) {
            'shelf' {
                $was = $it.Shelf
                for ($i = 0; $i -lt 4; $i++) { if ($Radios[$i].Checked) { $it.Shelf = $i } }
                # Made into episodes together: number them in list order.
                if ($it.Shelf -eq 1 -and $was -ne 1 -and $rows.Count -gt 1 -and -not $it.EpGuessed) { $n++; $it.Episode = $n }
                if ($it.Shelf -eq 1 -and $was -ne 1 -and $rows.Count -gt 1 -and $ShowBox.Text.Trim()) { $it.Show = $ShowBox.Text }
            }
            'show'    { $it.Show = $ShowBox.Text }
            'season'  { $it.Season = [int]$SeasonBox.Value }
            'episode' { if ($rows.Count -eq 1) { $it.Episode = [int]$EpBox.Value } }
            'title'   { if ($rows.Count -eq 1) { $it.Title = $TitleBox.Text } }
        }
        Update-Row $row
    }
    Update-Editor
}

$Table.Add_SelectedIndexChanged({ Load-Editor })
foreach ($r in $Radios) { $r.Add_CheckedChanged({ if ($this.Checked) { Save-Editor 'shelf' } }) }
$ShowBox.Add_TextChanged({ Save-Editor 'show' }); $ShowBox.Add_SelectedIndexChanged({ Save-Editor 'show' })
$SeasonBox.Add_ValueChanged({ Save-Editor 'season' }); $EpBox.Add_ValueChanged({ Save-Editor 'episode' })
$TitleBox.Add_TextChanged({ Save-Editor 'title' })

# ---- Step 3: posters ----
$P3 = New-Page
$P3.Controls.Add((New-Label 'Pick a poster' 0 4 366 $Head))
$PosterFor = New-Label '' 0 28 366; $P3.Controls.Add($PosterFor)
$Preview = New-Object Windows.Forms.PictureBox
$Preview.Location = New-Object Drawing.Point(0, 48); $Preview.Size = New-Object Drawing.Size(200, 113)
$Preview.BorderStyle = 'Fixed3D'; $Preview.SizeMode = 'Zoom'; $Preview.BackColor = [Drawing.Color]::Black
$P3.Controls.Add($Preview)
$P3.Controls.Add((New-Label 'Where from?' 210 46 150 $Bold))
$FromFrame = New-Object Windows.Forms.RadioButton; $FromFrame.Text = 'A frame from the video'
$FromOwn = New-Object Windows.Forms.RadioButton; $FromOwn.Text = 'My own picture...'
$FromNone = New-Object Windows.Forms.RadioButton; $FromNone.Text = 'No poster'
$py = 64
foreach ($r in @($FromFrame, $FromOwn, $FromNone)) {
    $r.Location = New-Object Drawing.Point(210, $py); $r.AutoSize = $true; $P3.Controls.Add($r); $py += 20
}
# A TV show's cover, asked once per show that doesn't have one yet.
$CoverLabel = New-Label 'Show cover:' 210 126 150 $Bold; $P3.Controls.Add($CoverLabel)
$CoverPic = New-Object Windows.Forms.PictureBox
$CoverPic.Location = New-Object Drawing.Point(210, 142); $CoverPic.Size = New-Object Drawing.Size(24, 36)
$CoverPic.BorderStyle = 'FixedSingle'; $CoverPic.SizeMode = 'Zoom'; $CoverPic.BackColor = [Drawing.Color]::White
$P3.Controls.Add($CoverPic)
$CoverTv = New-Button 'TVmaze' 238 142 46 18; $P3.Controls.Add($CoverTv)
$CoverOwn = New-Button 'Mine...' 286 142 46 18; $P3.Controls.Add($CoverOwn)
$CoverNone = New-Button 'None' 334 142 30 18; $P3.Controls.Add($CoverNone)
$CoverNote = New-Label '' 238 162 128; $CoverNote.ForeColor = $Dim; $P3.Controls.Add($CoverNote)

$P3.Controls.Add((New-Label 'Frames:' 0 168 100 $Bold))
$PrevFrames = New-Button '<' 0 186 20 40; $P3.Controls.Add($PrevFrames)
$NextFrames = New-Button '>' 344 186 20 40; $P3.Controls.Add($NextFrames)
$Thumbs = @(); $TimeLabels = @()
for ($i = 0; $i -lt 5; $i++) {
    $pb = New-Object Windows.Forms.PictureBox
    $pb.Location = New-Object Drawing.Point((24 + $i * 64), 186); $pb.Size = New-Object Drawing.Size(60, 40)
    $pb.SizeMode = 'Zoom'; $pb.BackColor = [Drawing.Color]::Black; $pb.BorderStyle = 'None'; $pb.Tag = $i; $pb.Cursor = [Windows.Forms.Cursors]::Hand
    $P3.Controls.Add($pb); $Thumbs += $pb
    $tl = New-Label '' (24 + $i * 64) 228 60; $tl.ForeColor = $Dim; $tl.TextAlign = 'TopCenter'; $P3.Controls.Add($tl); $TimeLabels += $tl
}
$ArrowNote = New-Label 'The arrows grab five more frames from further in.' 0 248; $ArrowNote.ForeColor = $Dim; $P3.Controls.Add($ArrowNote)
$SavedAs = New-Label '' 0 266 366; $P3.Controls.Add($SavedAs)
$PosterNote = New-Label 'Boro shows it on the video''s tile.' 0 284 366; $PosterNote.ForeColor = $Dim; $P3.Controls.Add($PosterNote)

$PosterAt = 0                 # which item step 3 is showing
$ShowCovers = @{}             # lower-case show name -> @{ Img; Note; Asked }

function Cover-For([string]$show) {
    $k = (Safe-Name $show).ToLower()
    if (-not $ShowCovers.ContainsKey($k)) { $ShowCovers[$k] = @{ Img = $null; Note = ''; Asked = $false } }
    return $ShowCovers[$k]
}

# The item that asks for a show's cover: the first one of that show in the batch.
function Asks-Cover($it) {
    if ($it.Shelf -ne 1 -or (Has-Cover $it.Show)) { return $false }
    $first = $Items | Where-Object { $_.Shelf -eq 1 -and (Safe-Name $_.Show) -ieq (Safe-Name $it.Show) } | Select-Object -First 1
    return [object]::ReferenceEquals($first, $it)
}

function Poster-Image($it) {
    switch ($it.Poster) {
        'frame' { if ($it.Frames.ContainsKey($it.FramePage)) { return $it.Frames[$it.FramePage][$it.FrameSel] } }
        'own'   { return $it.Own }
    }
    return $null
}

function Show-Poster {
    $it = $Items[$script:PosterAt]
    $script:Loading = $true
    $name = Boro-Name $it
    if ($it.Shelf -eq 1) { $name = "$($it.Show.Trim()) $name" }
    $PosterFor.Text = "$name   ($($script:PosterAt + 1) of $($Items.Count))"
    if (-not $it.Frames.ContainsKey($it.FramePage) -and -not $it.NoFrames) {
        $PosterFor.Text += '   - grabbing frames...'; $Form.Cursor = [Windows.Forms.Cursors]::WaitCursor; $Form.Refresh()
        Get-Frames $it $it.FramePage
        $Form.Cursor = [Windows.Forms.Cursors]::Default
        $PosterFor.Text = "$name   ($($script:PosterAt + 1) of $($Items.Count))"
        if ($it.NoFrames -and $it.Poster -eq 'frame') { $it.Poster = 'none' }
    }
    $imgs = $null; $times = $null
    if ($it.Frames.ContainsKey($it.FramePage)) { $imgs = $it.Frames[$it.FramePage]; $times = $it.Times[$it.FramePage] }
    for ($i = 0; $i -lt 5; $i++) {
        $Thumbs[$i].Image = $(if ($imgs) { $imgs[$i] } else { $null })
        $TimeLabels[$i].Text = $(if ($times -and $imgs[$i]) { Format-Time $times[$i] } else { '' })
        $sel = $it.Poster -eq 'frame' -and $i -eq $it.FrameSel
        $Thumbs[$i].BorderStyle = $(if ($sel) { 'Fixed3D' } else { 'None' })
        $Thumbs[$i].BackColor = $(if ($sel) { $Navy } else { [Drawing.Color]::Black })
        $Thumbs[$i].Padding = $(if ($sel) { New-Object Windows.Forms.Padding(2) } else { New-Object Windows.Forms.Padding(0) })
    }
    $FromFrame.Enabled = -not $it.NoFrames
    foreach ($c in @($PrevFrames, $NextFrames)) { $c.Enabled = -not $it.NoFrames }
    if ($it.NoFrames) {
        $ArrowNote.Text = "Couldn't read frames from this one. " + $(if ($FFmpeg) { 'Try your own picture.' } else { 'ffmpeg.exe next to me would help.' })
    } else { $ArrowNote.Text = 'The arrows grab five more frames from further in.' }
    $FromFrame.Checked = $it.Poster -eq 'frame'; $FromOwn.Checked = $it.Poster -eq 'own'; $FromNone.Checked = $it.Poster -eq 'none'
    $Preview.Image = Poster-Image $it
    $stem = [IO.Path]::GetFileNameWithoutExtension((Target-Path $it))
    if ($it.Poster -eq 'none') { $SavedAs.Text = 'No poster: Boro uses a frame of its own.' }
    else { $SavedAs.Text = "Saved as  $stem.jpg  next to the video." }

    $asks = Asks-Cover $it
    foreach ($c in @($CoverLabel, $CoverPic, $CoverTv, $CoverOwn, $CoverNone, $CoverNote)) { $c.Visible = $asks }
    if ($asks) {
        $cv = Cover-For $it.Show
        if (-not $cv.Asked) {
            $cv.Asked = $true
            $CoverNote.Text = 'Looking on TVmaze...'; $Form.Refresh()
            $cv.Img = Find-Cover $it.Show.Trim()
            $cv.Note = $(if ($cv.Img) { 'From TVmaze' } else { 'None found' })
        }
        $CoverPic.Image = $cv.Img; $CoverNote.Text = $cv.Note
    }
    if ($it.Shelf -ne 1) { $PosterNote.Text = 'Boro shows it on the video''s tile.' }
    elseif ($asks) { $PosterNote.Text = 'The cover is the show''s poster; this is the episode''s.' }
    else { $PosterNote.Text = 'This is the episode''s picture; the show has its cover.' }
    $script:Loading = $false
}

function Pick-Picture {
    $dlg = New-Object Windows.Forms.OpenFileDialog
    $dlg.Filter = 'Pictures|*.jpg;*.jpeg;*.png;*.bmp;*.gif|All files|*.*'
    if ($script:Folder) { $dlg.InitialDirectory = $script:Folder }
    if ($dlg.ShowDialog() -ne 'OK') { return $null }
    try { return Load-Image $dlg.FileName } catch {
        [Windows.Forms.MessageBox]::Show("That picture can't be read.", 'Boro Video Wizard', 'OK', 'Warning') | Out-Null
        return $null
    }
}

$FromFrame.Add_CheckedChanged({ if ($this.Checked -and -not $script:Loading) { $Items[$script:PosterAt].Poster = 'frame'; Show-Poster } })
$FromNone.Add_CheckedChanged({ if ($this.Checked -and -not $script:Loading) { $Items[$script:PosterAt].Poster = 'none'; Show-Poster } })
$FromOwn.Add_Click({
    $it = $Items[$script:PosterAt]
    $img = Pick-Picture
    if ($img) { $it.Own = $img }
    if ($it.Own) { $it.Poster = 'own' }   # cancelled with no picture: stays as it was
    Show-Poster
})
foreach ($pb in $Thumbs) {
    $pb.Add_Click({
        $it = $Items[$script:PosterAt]
        if (-not $this.Image) { return }
        $it.FrameSel = [int]$this.Tag; $it.Poster = 'frame'
        Show-Poster
    })
}
$PrevFrames.Add_Click({ $it = $Items[$script:PosterAt]; $it.FramePage = ($it.FramePage + $FramePages - 1) % $FramePages; Show-Poster })
$NextFrames.Add_Click({ $it = $Items[$script:PosterAt]; $it.FramePage = ($it.FramePage + 1) % $FramePages; Show-Poster })
$CoverTv.Add_Click({
    $it = $Items[$script:PosterAt]; $cv = Cover-For $it.Show
    $CoverNote.Text = 'Looking on TVmaze...'; $Form.Refresh()
    $cv.Img = Find-Cover $it.Show.Trim(); $cv.Note = $(if ($cv.Img) { 'From TVmaze' } else { 'None found' })
    Show-Poster
})
$CoverOwn.Add_Click({
    $img = Pick-Picture
    if ($img) { $cv = Cover-For $Items[$script:PosterAt].Show; $cv.Img = $img; $cv.Note = 'Your picture' }
    Show-Poster
})
$CoverNone.Add_Click({ $cv = Cover-For $Items[$script:PosterAt].Show; $cv.Img = $null; $cv.Note = 'No cover'; Show-Poster })

# ---- Step 4: brewing ----
$P4 = New-Page
$BrewHead = New-Label 'Brewing your videos...' 0 4 366 $Head; $P4.Controls.Add($BrewHead)
$BrewSub = New-Label 'Sit back. You can keep using your PC.' 0 30; $P4.Controls.Add($BrewSub)
$NowName = New-Label '' 0 54 366 $Bold; $P4.Controls.Add($NowName)
$Bar = New-Object Windows.Forms.ProgressBar
$Bar.Location = New-Object Drawing.Point(0, 72); $Bar.Size = New-Object Drawing.Size(360, 18); $Bar.Style = 'Blocks'; $Bar.Maximum = 1000
$P4.Controls.Add($Bar)
$NowInfo = New-Label '' 0 92; $NowInfo.ForeColor = $Dim; $P4.Controls.Add($NowInfo)
$AllLabel = New-Label '' 0 114 366 $Bold; $P4.Controls.Add($AllLabel)
$AllBar = New-Object Windows.Forms.ProgressBar
$AllBar.Location = New-Object Drawing.Point(0, 132); $AllBar.Size = New-Object Drawing.Size(360, 18); $AllBar.Style = 'Blocks'; $AllBar.Maximum = 1000
$P4.Controls.Add($AllBar)
$BrewList = New-Object Windows.Forms.ListBox
$BrewList.Location = New-Object Drawing.Point(0, 160); $BrewList.Size = New-Object Drawing.Size(360, 104); $BrewList.IntegralHeight = $false
$P4.Controls.Add($BrewList)
$DoneNote = New-Label "When it's done I'll open the PSP folder so you can copy" 0 270 366; $P4.Controls.Add($DoneNote)
$DoneNote2 = New-Label 'VIDEO onto the memory stick.' 0 286 366; $P4.Controls.Add($DoneNote2)

$Job = $null        # the conversion running
$BrewAt = 0         # index of the next item to start
$Paused = $false
$Brewing = $false
$Failed = 0

function Brew-Line($it) {
    $name = Boro-Name $it
    if ($it.Shelf -eq 1) { $name = "$($it.Show.Trim()) $name" }
    $where = $Shelves[$it.Shelf].Folder
    if ($it.Shelf -eq 1) { $where = "Shows\$(Safe-Name $it.Show)\Season $($it.Season)" }
    $mark = switch ($it.Status) { 'done' { 'OK' } 'busy' { '...' } 'failed' { 'X' } default { '' } }
    return ('{0,-4}{1}  ->  {2}' -f $mark, $name, $where)
}
function Fill-Brew {
    $BrewList.BeginUpdate(); $BrewList.Items.Clear()
    foreach ($it in $Items) { [void]$BrewList.Items.Add((Brew-Line $it)) }
    $BrewList.EndUpdate()
}

function Start-Next {
    if ($script:Job -or $script:Paused -or $script:BrewAt -ge $Items.Count) { return }
    $it = $Items[$script:BrewAt]; $script:BrewAt++
    if (-not (Test-Path -LiteralPath $it.Src)) { $it.Status = 'failed'; Write-Log "Gone: $($it.Src)"; Fill-Brew; return }
    $id = [guid]::NewGuid().ToString('N').Substring(0, 8)
    $out = Join-Path $WorkDir "$id.mp4"
    $json = Join-Path $WorkDir "$id.out.txt"
    $err = Join-Path $WorkDir "$id.err.txt"
    $hb = @(
        '-i', "`"$($it.Src)`"", '-o', "`"$out`"", '--json',
        '-f', 'av_mp4', '--optimize',
        '-e', 'x264', '--encoder-preset', 'medium', '--encoder-profile', 'main', '--encoder-level', '3.0', '-q', '22',
        '-x', 'ref=3:bframes=2:b-pyramid=none:weightp=0:8x8dct=0',
        '--maxWidth', '480', '--maxHeight', '272', '--non-anamorphic', '--modulus', '16',
        '-r', '30', '--pfr',
        '-a', '1', '-E', 'av_aac', '-B', '128', '--mixdown', 'stereo', '-R', '44.1',
        '--subtitle', 'none'
    )
    # Through cmd so its output goes to files the timer reads, with no window.
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = "$env:SystemRoot\System32\cmd.exe"
    $psi.Arguments = '/c ""{0}" {1} 1> "{2}" 2> "{3}""' -f $HandBrake, ($hb -join ' '), $json, $err
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $p = [Diagnostics.Process]::Start($psi)
    $script:Job = @{ Item = $it; Out = $out; Json = $json; Err = $err; Proc = $p; Progress = 0.0; Eta = -1 }
    $it.Status = 'busy'
    Fill-Brew
    Write-Log "Converting $($it.Src)"
}

# The last progress HandBrake wrote, from its JSON output.
function Read-Progress {
    $j = $script:Job
    try {
        $fs = [IO.File]::Open($j.Json, 'Open', 'Read', 'ReadWrite')
        $len = $fs.Length
        $from = [Math]::Max(0, $len - 4096)
        $fs.Seek($from, 'Begin') | Out-Null
        $buf = New-Object byte[] ($len - $from)
        $n = $fs.Read($buf, 0, $buf.Length)
        $fs.Close()
        $text = [Text.Encoding]::UTF8.GetString($buf, 0, $n)
        # Only the encode's progress: the scan before it reports its own.
        $m = [regex]::Matches($text, '"Working":\s*\{[^}]*?"Progress":\s*([0-9.eE\-]+)')
        if ($m.Count) { $j.Progress = [double]$m[$m.Count - 1].Groups[1].Value }
        $m = [regex]::Matches($text, '"Working":\s*\{[^}]*?"ETASeconds":\s*(\d+)')
        if ($m.Count) { $j.Eta = [int]$m[$m.Count - 1].Groups[1].Value }
    } catch { }
}

# Puts a converted video, its poster and its show's cover in place.
function File-Item($it, [string]$converted) {
    $dest = Unique-Path (Target-Path $it)
    New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
    Move-Item -LiteralPath $converted -Destination $dest
    $img = Poster-Image $it
    if ($img) {
        try { Save-Jpeg $img ([IO.Path]::ChangeExtension($dest, '.jpg')) 480 }
        catch { Write-Log "Couldn't save the poster for ${dest}: $($_.Exception.Message)" }
    }
    if ($it.Shelf -eq 1) {
        $cv = Cover-For $it.Show
        $coverPath = Join-Path (Split-Path (Split-Path $dest)) 'cover.jpg'
        if ($cv.Img -and -not (Has-Cover $it.Show)) {
            try { Save-Jpeg $cv.Img $coverPath 400 } catch { Write-Log "Couldn't save $coverPath" }
        }
    }
    New-Item -ItemType Directory -Force -Path $script:Originals | Out-Null
    Move-Item -LiteralPath $it.Src -Destination (Unique-Path (Join-Path $script:Originals (Split-Path $it.Src -Leaf)))
    Write-Log "Filed $dest"
}

function Finish-Job {
    $j = $script:Job
    $script:Job = $null
    $it = $j.Item
    $ok = $j.Proc.ExitCode -eq 0 -and (Test-Path -LiteralPath $j.Out) -and (Get-Item -LiteralPath $j.Out).Length -gt 0
    if ($ok) {
        try { File-Item $it $j.Out; $it.Status = 'done' }
        catch { $it.Status = 'failed'; $script:Failed++; Write-Log "Couldn't file $($it.Src): $($_.Exception.Message)" }
    } else {
        $it.Status = 'failed'; $script:Failed++
        $why = ''
        if (Test-Path -LiteralPath $j.Err) { $why = (Get-Content -LiteralPath $j.Err -Tail 3 -ErrorAction SilentlyContinue) -join ' ' }
        Write-Log "FAILED $($it.Src) (exit $($j.Proc.ExitCode)) $why"
        Remove-Item -LiteralPath $j.Out -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $j.Json, $j.Err -Force -ErrorAction SilentlyContinue
    Fill-Brew
}

function Stop-Job {
    if (-not $script:Job) { return }
    if (-not $script:Job.Proc.HasExited) { & taskkill.exe /PID $script:Job.Proc.Id /T /F 2>&1 | Out-Null }
    Start-Sleep -Milliseconds 300
    Remove-Item -LiteralPath $script:Job.Out, $script:Job.Json, $script:Job.Err -Force -ErrorAction SilentlyContinue
    $script:Job.Item.Status = ''
    $script:Job = $null
}

function Brew-Done {
    $script:Brewing = $false
    $Timer.Stop()
    $ok = @($Items | Where-Object { $_.Status -eq 'done' }).Count
    $BrewHead.Text = 'All done!'
    $BrewSub.Text = "$ok of $($Items.Count) ready for the PSP."
    if ($script:Failed) { $BrewSub.Text += " $($script:Failed) didn't work (see PSP\BoroWizard.log)." }
    $NowName.Text = ''; $NowInfo.Text = ''; $Bar.Value = 1000; $AllBar.Value = 1000
    $AllLabel.Text = "All videos:  $($Items.Count) of $($Items.Count)"
    $DoneNote.Text = 'Copy the VIDEO folder in PSP onto the memory stick'
    $DoneNote2.Text = 'and let it merge with the one there.'
    Update-Nav
    if ($ok) { Start-Process explorer.exe -ArgumentList ('"{0}"' -f (Split-Path $script:OutRoot)) }
}

$Timer = New-Object Windows.Forms.Timer
$Timer.Interval = 1000
$Timer.Add_Tick({
    if ($script:Job) { if ($script:Job.Proc.HasExited) { Finish-Job } else { Read-Progress } }
    Start-Next
    if (-not $script:Job -and $script:BrewAt -ge $Items.Count) { Brew-Done; return }
    $n = $Items.Count
    $done = @($Items | Where-Object { $_.Status -eq 'done' -or $_.Status -eq 'failed' }).Count
    $pc = 0.0
    if ($script:Job) {
        $it = $script:Job.Item
        $name = Boro-Name $it; if ($it.Shelf -eq 1) { $name = "$($it.Show.Trim()) $name" }
        $NowName.Text = "Now:  $name"
        $pc = [Math]::Max(0, [Math]::Min(1, $script:Job.Progress))
        $info = '{0:0}%' -f ($pc * 100)
        if ($script:Job.Eta -ge 0) { $info += '  -  about {0} min left' -f [Math]::Max(1, [Math]::Ceiling($script:Job.Eta / 60)) }
        $NowInfo.Text = $info
    } else {
        $NowName.Text = $(if ($script:Paused) { 'Paused.' } else { '' }); $NowInfo.Text = ''
    }
    $Bar.Value = [int]($pc * 1000)
    $AllLabel.Text = "All videos:  $([Math]::Min($n, $done + [int][bool]$script:Job)) of $n"
    $AllBar.Value = [int](1000 * [Math]::Min(1, ($done + $pc) / [Math]::Max(1, $n)))
})

function Start-Brew {
    $script:BrewAt = 0; $script:Failed = 0; $script:Paused = $false; $script:Brewing = $true
    foreach ($it in $Items) { $it.Status = '' }
    $BrewHead.Text = 'Brewing your videos...'; $BrewSub.Text = 'Sit back. You can keep using your PC.'
    $DoneNote.Text = "When it's done I'll open the PSP folder so you can copy"; $DoneNote2.Text = 'VIDEO onto the memory stick.'
    $Bar.Value = 0; $AllBar.Value = 0; $NowName.Text = ''; $NowInfo.Text = ''; $AllLabel.Text = "All videos:  0 of $($Items.Count)"
    Fill-Brew
    Write-Log "Brewing $($Items.Count) videos from $($script:Folder) with $HandBrake"
    $Timer.Start()
}

# ---- Moving between steps ----

function Update-Nav {
    $StepLabel.Text = "Step $script:Step of 4"
    $BackBtn.Visible = $true; $NextBtn.Visible = $true; $NextBtn.Enabled = $true; $BackBtn.Enabled = $script:Step -gt 1
    $NextBtn.Text = 'Next >'; $CancelBtn.Text = 'Cancel'
    switch ($script:Step) {
        1 { $NextBtn.Enabled = $FileList.CheckedIndices.Count -gt 0 }
        3 { if ($script:PosterAt -ge $Items.Count - 1) { $NextBtn.Text = 'Brew!' } }
        4 {
            $BackBtn.Visible = $false
            if ($script:Brewing) {
                $NextBtn.Text = $(if ($script:Paused) { 'Resume' } else { 'Pause' }); $CancelBtn.Text = 'Stop'
            } else { $NextBtn.Text = 'More...'; $CancelBtn.Text = 'Close' }
        }
    }
    $Form.AcceptButton = $null
}

function Go([int]$step) {
    $script:Step = $step
    foreach ($p in @($P1, $P2, $P3, $P4)) { $p.Visible = $false }
    switch ($step) {
        1 { Fill-Files; $P1.Visible = $true }
        2 { $P2.Visible = $true }
        3 { $P3.Visible = $true; Show-Poster }
        4 { $P4.Visible = $true }
    }
    Update-Nav
}

# Step 1 -> 2: the ticked videos, keeping anything already filled in.
function Take-Ticked {
    $Items.Clear()
    foreach ($i in $FileList.CheckedIndices) {
        $path = $script:Files[$i]
        if (-not $All.ContainsKey($path)) { $All[$path] = New-Item-For $path }
        [void]$Items.Add($All[$path])
    }
}

$NextBtn.Add_Click({
    switch ($script:Step) {
        1 { Take-Ticked; Go 2; Fill-Table; Load-Editor }
        2 {
            $need = @($Items | Where-Object { Missing $_ })
            if ($need.Count) {
                $first = $need[0]
                foreach ($row in $Table.Items) { if ([object]::ReferenceEquals($row.Tag, $first)) { $row.Selected = $true; $row.EnsureVisible() } }
                $what = Missing $first
                [Windows.Forms.MessageBox]::Show("$($need.Count) still need a $what (the red rows).", 'Boro Video Wizard', 'OK', 'Information') | Out-Null
                return
            }
            $script:PosterAt = 0; Go 3
        }
        3 {
            if ($script:PosterAt -lt $Items.Count - 1) { $script:PosterAt++; Show-Poster; Update-Nav }
            else { Go 4; Start-Brew; Update-Nav }
        }
        4 {
            if ($script:Brewing) { $script:Paused = -not $script:Paused; Update-Nav }
            else { $All.Clear(); $Items.Clear(); Go 1 }
        }
    }
})
$BackBtn.Add_Click({
    switch ($script:Step) {
        2 { Go 1 }
        3 {
            if ($script:PosterAt -gt 0) { $script:PosterAt--; Show-Poster; Update-Nav }
            else { Go 2; Fill-Table; Load-Editor }
        }
    }
})
$CancelBtn.Add_Click({
    if ($script:Step -eq 4 -and $script:Brewing) {
        $a = [Windows.Forms.MessageBox]::Show("Stop after this?`n`nYes: stop now (the video being converted is left as it was).`nNo: keep going.",
            'Boro Video Wizard', 'YesNo', 'Question')
        if ($a -ne 'Yes') { return }
        Stop-Job
        $script:BrewAt = $Items.Count
        Brew-Done
        return
    }
    $Form.Close()
})
$Form.Add_FormClosing({
    param($s, $e)
    if ($script:Job -and -not $script:Job.Proc.HasExited) {
        $a = [Windows.Forms.MessageBox]::Show("A video is still converting. Quit anyway?", 'Boro Video Wizard', 'YesNo', 'Question')
        if ($a -ne 'Yes') { $e.Cancel = $true; return }
        Stop-Job
    }
})

Go 1
[void]$Form.ShowDialog()
$Timer.Stop()
Write-Log 'Closed'
