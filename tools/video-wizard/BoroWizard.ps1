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
#        VIDEO\Movies\<Title> (1999).mp4     + <Title> (1999).jpg
#        VIDEO\Movies\<Show> (2000)\cover.jpg
#        VIDEO\Movies\<Show> (2000)\Season N\<Show> S01E03 <Episode title>.mp4 + .jpg
#        VIDEO\Music\<Artist - Song>.mp4     + .jpg
#        VIDEO\Clips\<Title>.mp4             + .jpg
#
# Originals move to <folder>\Originals. Copy PSP\VIDEO onto the memory stick
# (merging folders). Start it with Start-BoroWizard.bat.

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName Microsoft.VisualBasic
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# ---- Settings ---------------------------------------------------------------

$SettingsFile = Join-Path $PSScriptRoot 'BoroWizard.settings.txt'
$WorkDir   = Join-Path $env:TEMP 'BoroWizard'
$VideoExts = @('.mp4', '.mkv', '.avi', '.mov', '.m4v', '.wmv', '.flv', '.webm', '.mpg', '.mpeg', '.ts')
$FramePages = 4          # 5 frames a page, spread through the video

$Shelves = @(
    @{ Name = 'Movie';       Short = 'Movie'; Folder = 'Movies' },
    @{ Name = 'TV episode';  Short = 'TV';    Folder = 'Movies' },   # shows live among the movies
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
             Duration = -1.0; Clip = $null; NoFrames = $false; Status = ''; EpGuessed = $false; Part = $null; Year = '' }
    if ($base -match '^(.*?)[\s._\-]*[Ss](\d{1,2})[\s._]?[Ee](\d{1,3})(.*)$' -or
        $base -match '^(.*?)[\s._\-]+(\d{1,2})x(\d{2,3})(.*)$') {
        $it.Shelf = 1
        $it.Show = Clean-Name ($Matches[1] -replace '[\s._\-]*\(?(19|20)\d{2}\)?$', '')
        $it.Season = [Math]::Max(1, [Math]::Min(99, [int]$Matches[2]))
        $it.Episode = [Math]::Max(1, [Math]::Min(999, [int]$Matches[3]))
        $it.Title = Clean-Name (" " + $Matches[4])
        $it.EpGuessed = $true
        if (-not $it.Show) { $it.Show = Clean-Name (Split-Path (Split-Path $path) -Leaf) }
        $known = Existing-Shows | Where-Object { $_ -ieq $it.Show } | Select-Object -First 1
        if ($known) { $it.Show = $known }
        return $it
    }
    # Just "Episode 2" / "Ep 02" / "E02": the folder is usually named after the show.
    if ($base -match '^(?:episode|ep|e)[\s._\-]*(\d{1,3})\b[\s._\-]*(.*)$') {
        $it.Shelf = 1
        $it.Show = Clean-Name (Split-Path (Split-Path $path) -Leaf)
        $it.Episode = [Math]::Max(1, [int]$Matches[1])
        $it.Title = Clean-Name (" " + $Matches[2])
        $it.EpGuessed = $true
        return $it
    }
    # "Show - Episode 3 - Title", "Show Ep 03", "Show E03"
    if ($base -match '^(.+?)[\s._\-]+(?:episode|ep|e)[\s._]*(\d{1,3})\b[\s._\-]*(.*)$') {
        $it.Shelf = 1
        $it.Show = Clean-Name $Matches[1]
        $it.Episode = [Math]::Max(1, [int]$Matches[2])
        $it.Title = Clean-Name (" " + $Matches[3])
        $it.EpGuessed = $true
        if (-not $it.Show) { $it.Show = Clean-Name (Split-Path (Split-Path $path) -Leaf) }
        $known = Existing-Shows | Where-Object { $_ -ieq $it.Show } | Select-Object -First 1
        if ($known) { $it.Show = $known }
        return $it
    }
    if ($base -match '(?<!\d)(19\d{2}|20\d{2})(?!\d)') { $it.Year = $Matches[1] }
    $t = Clean-Name $base
    $t = ($t -replace '\s*\(?\b(19|20)\d{2}\b\)?\s*$', '').Trim()
    if (-not $t) { $t = Clean-Name $base }
    if ($base -match ' - ' -and $base -notmatch '(?i)\b(19|20)\d{2}\b') { $it.Shelf = 2 }
    $it.Title = $t
    return $it
}

function Strip-Year([string]$s) { return ($s -replace '\s*[\(\[](18|19|20)\d{2}[\)\]]\s*$', '').Trim() }

# Show folders already in the output, anywhere under VIDEO: lower-case name
# (without its year) -> @{ Name; Path }.
function Show-Dirs {
    $map = @{}
    if (-not $script:OutRoot -or -not (Test-Path -LiteralPath $script:OutRoot)) { return $map }
    foreach ($e in (Find-Coverables $script:OutRoot)) {
        if ($e.Kind -ne 'show') { continue }
        $n = Strip-Year (Split-Path $e.Path -Leaf)
        if (-not $map.ContainsKey($n.ToLower())) { $map[$n.ToLower()] = @{ Name = $n; Path = $e.Path } }
    }
    return $map
}

function Existing-Shows { return @((Show-Dirs).Values | ForEach-Object { $_.Name } | Sort-Object) }

# The year a show started, from TVmaze, or ''.
$ShowYears = @{}
function Show-Year([string]$show) {
    $k = $show.Trim().ToLower()
    if (-not $k) { return '' }
    if ($ShowYears.ContainsKey($k)) { return $ShowYears[$k] }
    $y = ''
    try {
        $r = Invoke-RestMethod -Uri ("https://api.tvmaze.com/singlesearch/shows?q=" + [uri]::EscapeDataString($show.Trim())) -TimeoutSec 8
        if ($r.premiered -and $r.premiered.Length -ge 4) { $y = $r.premiered.Substring(0, 4) }
    } catch { }
    $ShowYears[$k] = $y
    return $y
}

# Where a show's folder is: the one already there, or a new
# VIDEO\Movies\<Show> (<year>) beside the movies.
function Show-Dir([string]$show) {
    $n = Safe-Name (Strip-Year $show)
    $m = Show-Dirs
    if ($m.ContainsKey($n.ToLower())) { return $m[$n.ToLower()].Path }
    $y = Show-Year $n
    $leaf = $(if ($y) { "$n ($y)" } else { $n })
    return Join-Path (Join-Path $script:OutRoot 'Movies') $leaf
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
    if ($it.Shelf -ne 1 -and $it.Year) { return "$($it.Title.Trim()) ($($it.Year))" }
    return $it.Title.Trim()
}

function Target-Path($it) {
    $folder = Join-Path $script:OutRoot $Shelves[$it.Shelf].Folder
    $title = Safe-Name $it.Title
    if ($it.Shelf -eq 1) {
        $show = Safe-Name (Strip-Year $it.Show)
        $file = '{0} S{1:D2}E{2:D2}' -f $show, [int]$it.Season, [int]$it.Episode
        if ($title) { $file += " $title" }
        return Join-Path (Join-Path (Show-Dir $it.Show) ("Season {0}" -f [int]$it.Season)) "$file.mp4"
    }
    # Boro shows the year from "(1999)" at the end of the name.
    if ($it.Shelf -ne 1 -and $it.Year -match '^\d{4}$') { $title += " ($($it.Year))" }
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
    try { return [Drawing.Image]::FromStream($ms) } catch { }
    # WebP, AVIF or HEIC (often saved from the web with a .jpg name): let Windows decode it.
    $img = Load-ViaWindows $path
    if ($img) { return $img }
    throw "Can't read $path"
}

# Decodes any picture Windows' Photos app can open, as a PNG in memory.
function Load-ViaWindows([string]$path) {
    if (-not (Init-WinRT)) { return $null }
    try {
        $null = [Windows.Graphics.Imaging.BitmapDecoder, Windows.Graphics, ContentType = WindowsRuntime]
        $null = [Windows.Graphics.Imaging.BitmapEncoder, Windows.Graphics, ContentType = WindowsRuntime]
        $null = [Windows.Graphics.Imaging.SoftwareBitmap, Windows.Graphics, ContentType = WindowsRuntime]
        $null = [Windows.Storage.Streams.InMemoryRandomAccessStream, Windows.Storage.Streams, ContentType = WindowsRuntime]
        $file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($path)) ([Windows.Storage.StorageFile])
        $in = Await ($file.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
        $dec = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($in)) ([Windows.Graphics.Imaging.BitmapDecoder])
        $bmp = Await ($dec.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])
        $bmp = [Windows.Graphics.Imaging.SoftwareBitmap]::Convert($bmp, [Windows.Graphics.Imaging.BitmapPixelFormat]::Bgra8,
            [Windows.Graphics.Imaging.BitmapAlphaMode]::Premultiplied)
        $mem = New-Object Windows.Storage.Streams.InMemoryRandomAccessStream
        $enc = Await ([Windows.Graphics.Imaging.BitmapEncoder]::CreateAsync([Windows.Graphics.Imaging.BitmapEncoder]::PngEncoderId, $mem)) ([Windows.Graphics.Imaging.BitmapEncoder])
        $enc.SetSoftwareBitmap($bmp)
        Await-Action ($enc.FlushAsync())
        $mem.Seek(0)
        $rs = [IO.WindowsRuntimeStreamExtensions]::AsStreamForRead($mem)
        $ms = New-Object IO.MemoryStream
        $rs.CopyTo($ms); $rs.Dispose(); $in.Dispose()
        $ms.Position = 0
        return [Drawing.Image]::FromStream($ms)
    } catch {
        Write-Log "Windows couldn't read ${path}: $($_.Exception.Message)"
        return $null
    }
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

# TVmaze's posters for a show, best first, as picture URLs. TVmaze's main
# picture for a show is sometimes a still from an episode, so the proper
# posters from its image list come first.
function Find-Posters([string]$show) {
    $urls = @()
    try {
        $q = [uri]::EscapeDataString($show)
        $r = Invoke-RestMethod -Uri "https://api.tvmaze.com/singlesearch/shows?q=$q" -TimeoutSec 8
        try {
            $imgs = @(Invoke-RestMethod -Uri "https://api.tvmaze.com/shows/$($r.id)/images" -TimeoutSec 8)
            $posters = @($imgs | Where-Object { $_.type -eq 'poster' } | Sort-Object { -not $_.main })
            foreach ($p in $posters) {
                $u = $p.resolutions.original.url; if (-not $u) { $u = $p.resolutions.medium.url }
                if ($u) { $urls += $u }
            }
        } catch { }
        # The main picture last: sometimes it's a still, not a poster.
        $main = $r.image.original; if (-not $main) { $main = $r.image.medium }
        if ($main -and $urls -notcontains $main) { $urls += $main }
    } catch { Write-Log "No posters from TVmaze for '$show': $($_.Exception.Message)" }
    return $urls
}

function Download-Image([string]$url) {
    try {
        $tmp = Join-Path $WorkDir 'cover-download'
        Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing -TimeoutSec 15
        return Load-Image $tmp
    } catch { Write-Log "Couldn't download ${url}: $($_.Exception.Message)"; return $null }
}

# The best TVmaze poster for a show, as a picture, or $null.
function Find-Cover([string]$show) {
    foreach ($u in (Find-Posters $show)) {
        $img = Download-Image $u
        if ($img -and $img.Height -ge $img.Width) { return $img }   # skip landscape stills
    }
    return $null
}

function Has-Cover([string]$show) {
    $dir = Show-Dir $show
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
function Await-Action($op) {
    $m = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction' } |
        Select-Object -First 1
    $t = $m.Invoke($null, @($op))
    if (-not $t.Wait(20000)) { throw 'Windows took too long' }
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
    $off = 0.0
    # A part of a split file: frames from its own stretch.
    if ($d -gt 0 -and $it.Part) { $off = $it.Part.Start; $d = $(if ($it.Part.Len -gt 0) { [Math]::Min($it.Part.Len, $d - $off) } else { $d - $off }) }
    $imgs = @(); $times = @()
    for ($i = 0; $i -lt 5; $i++) {
        $sec = 0.0
        if ($d -gt 0) { $sec = $off + $d * ($page * 5 + $i + 1) / ($FramePages * 5 + 1) }
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
$RestBtn = New-Button 'Frames for the rest >>' 150 330 120; $RestBtn.Visible = $false; $Form.Controls.Add($RestBtn)

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
$Remember = New-Label "I'll remember this folder for next time." 0 280 250; $Remember.ForeColor = $Dim; $P1.Controls.Add($Remember)
$CoversBtn = New-Button 'Covers...' 260 276 100; $P1.Controls.Add($CoversBtn)
$Files = @()     # full paths, in list order

function Fill-Files {
    $FileList.Items.Clear(); $script:Files = @()
    if (-not $script:Folder) {
        $FolderBox.Text = ''; $FoundLabel.Text = 'Pick a folder to start.'; $OutLabel.Text = ''
        return
    }
    $FolderBox.Text = $script:Folder
    $vids = @(Get-ChildItem -LiteralPath $script:Folder -File -ErrorAction SilentlyContinue |
        Where-Object { $VideoExts -contains $_.Extension.ToLower() } |
        Sort-Object { [regex]::Replace($_.Name, '\d+', { $args[0].Value.PadLeft(8, '0') }) })
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
# ---- Show covers: fix the posters of shows you already have ----
$EpisodeName = '(?i)(s\d{1,2}\s*e\d{1,3}|\b\d{1,2}x\d{2,3}\b|(^|[\s._\-])(episode|ep|e)[\s._\-]*\d{1,3}\b)'

# A file name with a short episode-like number in it ("FLCL 01", "Show - 3"),
# not a year or "x264"/"480p".
$ShortNumber = '(?<![A-Za-z0-9])\d{1,3}(?![A-Za-z0-9])'

function Is-EpisodeFile($f) {
    if ($VideoExts -notcontains $f.Extension.ToLower()) { return $false }
    $base = [IO.Path]::GetFileNameWithoutExtension($f.Name)
    return $base -match $EpisodeName -or $base -match $ShortNumber
}

# What under a folder can have a cover, at any depth: show folders (season
# folders inside, or two or more numbered episodes; the same rule Boro uses)
# and single videos (movies), which get <name>.jpg beside them.
function Find-Coverables([string]$root, [int]$depth = 0) {
    $out = @()
    if ($depth -gt 5) { return $out }
    foreach ($f in @(Get-ChildItem -LiteralPath $root -File -ErrorAction SilentlyContinue)) {
        if ($VideoExts -contains $f.Extension.ToLower()) { $out += @{ Kind = 'movie'; Path = $f.FullName } }
    }
    foreach ($d in @(Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue)) {
        if ($d.Name.StartsWith('.')) { continue }
        $seasons = @(Get-ChildItem -LiteralPath $d.FullName -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^(?i)(season|series)\s*\d+$|^[sS]\d+$' }).Count
        $eps = @(Get-ChildItem -LiteralPath $d.FullName -File -ErrorAction SilentlyContinue | Where-Object { Is-EpisodeFile $_ }).Count
        if ($seasons -gt 0 -or $eps -ge 2) { $out += @{ Kind = 'show'; Path = $d.FullName } }
        else { $out += Find-Coverables $d.FullName ($depth + 1) }
    }
    return $out
}

function Cover-Path($e) {
    if ($e.Kind -eq 'show') { return Join-Path $e.Path 'cover.jpg' }
    return [IO.Path]::ChangeExtension($e.Path, '.jpg')
}
function Cover-Name($e) {
    if ($e.Kind -eq 'show') { return Clean-Name (Split-Path $e.Path -Leaf) }
    $n = Clean-Name ([IO.Path]::GetFileNameWithoutExtension($e.Path))
    return ($n -replace '\s*\(?\b(19|20)\d{2}\b\)?\s*$', '').Trim()
}

# Movie posters from the iTunes store (no sign-up needed), best first.
function Find-MoviePosters([string]$movie) {
    $urls = @()
    try {
        $q = [uri]::EscapeDataString($movie)
        $r = Invoke-RestMethod -Uri "https://itunes.apple.com/search?term=$q&media=movie&entity=movie&limit=6" -TimeoutSec 8
        foreach ($m in $r.results) { if ($m.artworkUrl100) { $urls += ($m.artworkUrl100 -replace '\d+x\d+bb', '400x600bb') } }
    } catch { Write-Log "No posters from iTunes for '$movie': $($_.Exception.Message)" }
    return $urls
}

function Show-CoversDialog {
    $dlg = New-Object Windows.Forms.FolderBrowserDialog
    $dlg.Description = 'Pick the folder your videos are in. The VIDEO folder on the memory stick works too.'
    if ($script:Folder) { $dlg.SelectedPath = $script:Folder }
    if ($dlg.ShowDialog() -ne 'OK') { return }
    $Form.Cursor = [Windows.Forms.Cursors]::WaitCursor
    $found = @(Find-Coverables $dlg.SelectedPath | Sort-Object { $_.Kind -ne 'show' }, { Cover-Name $_ })
    $Form.Cursor = [Windows.Forms.Cursors]::Default
    if (-not $found.Count) {
        [Windows.Forms.MessageBox]::Show('No videos in there.', 'Boro Video Wizard', 'OK', 'Information') | Out-Null
        return
    }

    $d = New-Object Windows.Forms.Form
    $d.Text = 'Covers'; $d.ClientSize = New-Object Drawing.Size(430, 300)
    $d.FormBorderStyle = 'FixedDialog'; $d.MaximizeBox = $false; $d.MinimizeBox = $false
    $d.StartPosition = 'CenterParent'; $d.BackColor = $Gray; $d.Font = $Font
    $d.Controls.Add((New-Label 'Shows and movies:' 10 8 190 $Bold))
    $list = New-Object Windows.Forms.ListBox
    $list.Location = New-Object Drawing.Point(10, 26); $list.Size = New-Object Drawing.Size(190, 230); $list.IntegralHeight = $false
    $list.HorizontalScrollbar = $true
    $d.Controls.Add($list)
    $pic = New-Object Windows.Forms.PictureBox
    $pic.Location = New-Object Drawing.Point(212, 26); $pic.Size = New-Object Drawing.Size(120, 180)
    $pic.BorderStyle = 'Fixed3D'; $pic.SizeMode = 'Zoom'; $pic.BackColor = [Drawing.Color]::White
    $d.Controls.Add($pic)
    $info = New-Label '' 212 210 210; $info.ForeColor = $Dim; $d.Controls.Add($info)
    $prev = New-Button '<' 340 26 24; $d.Controls.Add($prev)
    $next = New-Button '>' 368 26 24; $d.Controls.Add($next)
    $search = New-Button 'Search as...' 340 54 82; $d.Controls.Add($search)
    $own = New-Button 'My own...' 340 82 82; $d.Controls.Add($own)
    $save = New-Button 'Save cover' 340 130 82; $d.Controls.Add($save)
    $hint = New-Label 'Shows get cover.jpg; movies get <name>.jpg.' 10 262 300; $hint.ForeColor = $Dim; $d.Controls.Add($hint)
    $close = New-Button 'Close' 345 268 75; $close.DialogResult = 'Cancel'; $d.Controls.Add($close); $d.CancelButton = $close

    # The poster URLs found, which one is showing, and the picture on screen.
    $st = @{ Urls = @(); At = 0; Img = $null; Label = ''; From = '' }
    $label = {
        param($e)
        $tag = $(if ($e.Kind -eq 'show') { 'Show' } else { 'Movie' })
        $n = "${tag}:  $(Cover-Name $e)"
        if (Test-Path -LiteralPath (Cover-Path $e)) { "$n   (has cover)" } else { $n }
    }
    foreach ($e in $found) { [void]$list.Items.Add((& $label $e)) }

    $showOne = {
        if (-not $st.Urls.Count) { $pic.Image = $st.Img; $info.Text = $st.Label; return }
        $d.Cursor = [Windows.Forms.Cursors]::WaitCursor
        $st.Img = Download-Image $st.Urls[$st.At]
        $d.Cursor = [Windows.Forms.Cursors]::Default
        $pic.Image = $st.Img
        $info.Text = "$($st.From) poster $($st.At + 1) of $($st.Urls.Count)"
    }
    $lookUp = {
        param($e, $name)
        $d.Cursor = [Windows.Forms.Cursors]::WaitCursor; $info.Text = 'Looking...'; $d.Refresh()
        if ($e.Kind -eq 'show') { $st.From = 'TVmaze'; $st.Urls = @(Find-Posters $name) }
        else { $st.From = 'iTunes'; $st.Urls = @(Find-MoviePosters $name) }
        # A show TVmaze doesn't know may be a film or special on iTunes, and the other way round.
        if (-not $st.Urls.Count -and $e.Kind -eq 'show') { $st.From = 'iTunes'; $st.Urls = @(Find-MoviePosters $name) }
        if (-not $st.Urls.Count -and $e.Kind -ne 'show') { $st.From = 'TVmaze'; $st.Urls = @(Find-Posters $name) }
        $st.At = 0; $st.Img = $null
        $d.Cursor = [Windows.Forms.Cursors]::Default
        if (-not $st.Urls.Count) { $st.Label = "Nothing found for '$name'. Try Search as..." }
        & $showOne
    }
    $list.Add_SelectedIndexChanged({
        if ($list.SelectedIndex -lt 0) { return }
        $e = $found[$list.SelectedIndex]
        $pic.Image = $null
        & $lookUp $e (Cover-Name $e)
        $cur = Cover-Path $e
        if (-not $st.Urls.Count -and (Test-Path -LiteralPath $cur)) { $st.Img = Load-Image $cur; $pic.Image = $st.Img; $info.Text = 'Its cover now' }
    })
    $prev.Add_Click({ if ($st.Urls.Count) { $st.At = ($st.At + $st.Urls.Count - 1) % $st.Urls.Count; & $showOne } })
    $next.Add_Click({ if ($st.Urls.Count) { $st.At = ($st.At + 1) % $st.Urls.Count; & $showOne } })
    $search.Add_Click({
        if ($list.SelectedIndex -lt 0) { return }
        $e = $found[$list.SelectedIndex]
        $name = [Microsoft.VisualBasic.Interaction]::InputBox('Search for:', 'Covers', (Cover-Name $e))
        if ($name.Trim()) { & $lookUp $e $name.Trim() }
    })
    $own.Add_Click({
        $img = Pick-Picture
        if ($img) { $st.Urls = @(); $st.Img = $img; $st.Label = 'Your picture'; & $showOne }
    })
    $save.Add_Click({
        if ($list.SelectedIndex -lt 0 -or -not $st.Img) { return }
        $e = $found[$list.SelectedIndex]
        try {
            Save-Jpeg $st.Img (Cover-Path $e) 400
            $list.Items[$list.SelectedIndex] = (& $label $e)
            $info.Text = 'Saved.'
            Write-Log "Cover saved: $(Cover-Path $e)"
        } catch { $info.Text = "Couldn't save: $($_.Exception.Message)" }
    })
    $list.SelectedIndex = 0
    [void]$d.ShowDialog($Form)
}
$CoversBtn.Add_Click({ Show-CoversDialog })

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
$Table.Location = New-Object Drawing.Point(0, 26); $Table.Size = New-Object Drawing.Size(360, 76)
$Table.View = 'Details'; $Table.FullRowSelect = $true; $Table.HideSelection = $false; $Table.MultiSelect = $true
$Table.HeaderStyle = 'Nonclickable'
[void]$Table.Columns.Add('File', 130); [void]$Table.Columns.Add('Shelf', 46); [void]$Table.Columns.Add('Name in Boro', 162)
$P2.Controls.Add($Table)
# Batch tools: they work on the selected rows, or every TV row when one or none is selected.
$TvBtn = New-Button 'Make them a TV show...' 0 104 150 19; $P2.Controls.Add($TvBtn)
$SplitBtn = New-Button 'Split into episodes...' 156 104 130 19; $P2.Controls.Add($SplitBtn)
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
# Everything but TV: shows get their year from TVmaze.
$YearLabel = New-Label 'Year:' 0 166 60 $Bold; $P2.Controls.Add($YearLabel)
$YearBox = New-Object Windows.Forms.TextBox
$YearBox.Location = New-Object Drawing.Point(0, 182); $YearBox.Size = New-Object Drawing.Size(50, 21); $YearBox.MaxLength = 4
$P2.Controls.Add($YearBox)
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
$ShowsAs = New-Label '' 0 250 366; $P2.Controls.Add($ShowsAs)
$Hint = New-Label 'Guessed from file names. Ctrl/Shift-click to edit several.' 0 272; $Hint.ForeColor = $Dim; $P2.Controls.Add($Hint)
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
        $file = Split-Path $it.Src -Leaf
        if ($it.Part) { $file = "$file  (part $($it.Part.N))" }
        $row = New-Object Windows.Forms.ListViewItem($file)
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
        $ShowBox.Text = $it.Show; $SeasonBox.Value = $it.Season; $EpBox.Value = $it.Episode; $TitleBox.Text = $it.Title; $YearBox.Text = $it.Year
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
    foreach ($c in @($YearLabel, $YearBox)) { $c.Visible = $it -and $it.Shelf -ne 1 }
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
            'show'    { $it.Show = $ShowBox.Text; $it.Shelf = 1 }
            'season'  { $it.Season = [int]$SeasonBox.Value; $it.Shelf = 1 }
            'episode' { if ($rows.Count -eq 1) { $it.Episode = [int]$EpBox.Value } }
            'title'   { if ($rows.Count -eq 1) { $it.Title = $TitleBox.Text } }
            'year'    { $it.Year = $YearBox.Text.Trim() }
        }
        Update-Row $row
    }
    Update-Editor
}

$Table.Add_SelectedIndexChanged({ Load-Editor })
$Table.Add_KeyDown({
    param($s, $e)
    if ($e.Control -and $e.KeyCode -eq 'A') { foreach ($row in $Table.Items) { $row.Selected = $true }; $e.Handled = $true }
})

# The rows a batch tool works on.
function Batch-Rows([bool]$tvOnly) {
    $rows = Selected-Rows
    if ($rows.Count -lt 2) { $rows = @($Table.Items | ForEach-Object { $_ }) }
    if ($tvOnly) { $rows = @($rows | Where-Object { $_.Tag.Shelf -eq 1 }) }
    return $rows
}

$EpisodeLists = @{}     # lower-case show -> @{ 'SxE' = name }, or $null if TVmaze didn't know it
function Find-Episodes([string]$show) {
    $k = $show.ToLower()
    if ($EpisodeLists.ContainsKey($k)) { return $EpisodeLists[$k] }
    $map = $null
    try {
        $q = [uri]::EscapeDataString($show)
        $r = Invoke-RestMethod -Uri "https://api.tvmaze.com/singlesearch/shows?q=$q&embed=episodes" -TimeoutSec 10
        $map = @{}
        foreach ($e in $r._embedded.episodes) { if ($e.number) { $map["$($e.season)x$($e.number)"] = [string]$e.name } }
    } catch { Write-Log "No episode list from TVmaze for '$show': $($_.Exception.Message)" }
    $EpisodeLists[$k] = $map
    return $map
}

# One dialog sets up a batch of episodes: show, season, numbers and titles.
function Show-TvDialog {
    $rows = Batch-Rows $false
    if (-not $rows.Count) { return }
    $d = New-Object Windows.Forms.Form
    $d.Text = 'Make them a TV show'; $d.ClientSize = New-Object Drawing.Size(300, 214)
    $d.FormBorderStyle = 'FixedDialog'; $d.MaximizeBox = $false; $d.MinimizeBox = $false
    $d.StartPosition = 'CenterParent'; $d.BackColor = $Gray; $d.Font = $Font
    $d.Controls.Add((New-Label "These $($rows.Count) videos are episodes of:" 10 10 280 $Bold))
    $show = New-Object Windows.Forms.ComboBox
    $show.Location = New-Object Drawing.Point(10, 30); $show.Size = New-Object Drawing.Size(280, 21)
    foreach ($x in $ShowBox.Items) { [void]$show.Items.Add($x) }
    $first = $rows | ForEach-Object { $_.Tag } | Where-Object { $_.Show.Trim() } | Select-Object -First 1
    $show.Text = $(if ($first) { $first.Show.Trim() } else { Split-Path $script:Folder -Leaf })
    $d.Controls.Add($show)
    $d.Controls.Add((New-Label 'Season:' 10 62 60))
    $season = New-Object Windows.Forms.NumericUpDown
    $season.Location = New-Object Drawing.Point(70, 60); $season.Size = New-Object Drawing.Size(50, 21); $season.Minimum = 1; $season.Maximum = 99
    $season.Value = [Math]::Max(1, [int]$rows[0].Tag.Season)
    $d.Controls.Add($season)
    $guessed = -not ($rows | Where-Object { -not $_.Tag.EpGuessed })
    $keep = New-Object Windows.Forms.RadioButton
    $keep.Text = 'Episode numbers from the file names'; $keep.Location = New-Object Drawing.Point(10, 90); $keep.AutoSize = $true
    $keep.Enabled = $guessed; $keep.Checked = $guessed
    $d.Controls.Add($keep)
    $count = New-Object Windows.Forms.RadioButton
    $count.Text = 'Number them in list order, from'; $count.Location = New-Object Drawing.Point(10, 112); $count.AutoSize = $true
    $count.Checked = -not $guessed
    $d.Controls.Add($count)
    $start = New-Object Windows.Forms.NumericUpDown
    $start.Location = New-Object Drawing.Point(206, 110); $start.Size = New-Object Drawing.Size(50, 21); $start.Minimum = 1; $start.Maximum = 999
    $d.Controls.Add($start)
    $titles = New-Object Windows.Forms.CheckBox
    $titles.Text = 'Get episode titles from TVmaze'; $titles.Location = New-Object Drawing.Point(10, 142); $titles.AutoSize = $true; $titles.Checked = $true
    $d.Controls.Add($titles)
    $ok = New-Button 'OK' 134 180; $ok.DialogResult = 'OK'; $d.Controls.Add($ok); $d.AcceptButton = $ok
    $no = New-Button 'Cancel' 215 180; $no.DialogResult = 'Cancel'; $d.Controls.Add($no); $d.CancelButton = $no
    if ($d.ShowDialog($Form) -ne 'OK' -or -not $show.Text.Trim()) { return }

    $name = $show.Text.Trim()
    $known = Existing-Shows | Where-Object { $_ -ieq $name } | Select-Object -First 1
    if ($known) { $name = $known }
    $map = $null
    if ($titles.Checked) { $Form.Cursor = [Windows.Forms.Cursors]::WaitCursor; $map = Find-Episodes $name; $Form.Cursor = [Windows.Forms.Cursors]::Default }
    $n = [int]$start.Value
    foreach ($row in $rows) {
        $it = $row.Tag
        $it.Shelf = 1; $it.Show = $name; $it.Season = [int]$season.Value
        if ($count.Checked) { $it.Episode = $n; $n++ }
        $key = "$($it.Season)x$($it.Episode)"
        if ($map -and $map.ContainsKey($key) -and $map[$key]) { $it.Title = Safe-Name $map[$key] }
        elseif ($it.Title -match '^(?i)(episode|ep|e)\s*\d+$') { $it.Title = '' }   # "Episode 2" says nothing new
        Update-Row $row
    }
    if (-not $ShowBox.Items.Contains($name)) { [void]$ShowBox.Items.Add($name) }
    Load-Editor
    if ($titles.Checked -and -not $map) {
        [Windows.Forms.MessageBox]::Show("TVmaze didn't know '$name', so the episodes have no titles. That's fine: Boro shows their numbers.",
            'Boro Video Wizard', 'OK', 'Information') | Out-Null
    }
}
$TvBtn.Add_Click({ Show-TvDialog })

# ---- Splitting one file into episodes ----

# A file's length and chapters (start and length in seconds), from HandBrake's scan.
function Scan-Chapters([string]$path) {
    $res = @{ Duration = 0.0; Chapters = @() }
    try {
        $psi = New-Object Diagnostics.ProcessStartInfo
        $psi.FileName = $HandBrake; $psi.Arguments = ('-i "{0}" --scan -t 1 --json' -f $path)
        $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
        $p = [Diagnostics.Process]::Start($psi)
        $out = $p.StandardOutput.ReadToEndAsync(); $null = $p.StandardError.ReadToEndAsync()
        if (-not $p.WaitForExit(90000)) { try { $p.Kill() } catch { } }
        $text = $out.Result
        $at = $text.IndexOf('JSON Title Set:')
        if ($at -lt 0) { return $res }
        # The JSON object after the marker, by matching braces.
        $start = $text.IndexOf('{', $at); $depth = 0; $end = -1; $inStr = $false
        for ($i = $start; $i -lt $text.Length; $i++) {
            $c = $text[$i]
            if ($inStr) { if ($c -eq '\') { $i++ } elseif ($c -eq '"') { $inStr = $false }; continue }
            if ($c -eq '"') { $inStr = $true } elseif ($c -eq '{') { $depth++ } elseif ($c -eq '}') { $depth--; if ($depth -eq 0) { $end = $i; break } }
        }
        if ($end -lt 0) { return $res }
        $j = $text.Substring($start, $end - $start + 1) | ConvertFrom-Json
        $t = $j.TitleList[0]
        $secs = { param($x) if ($x.Ticks) { $x.Ticks / 90000.0 } else { $x.Hours * 3600 + $x.Minutes * 60 + $x.Seconds } }
        $res.Duration = & $secs $t.Duration
        $at = 0.0
        foreach ($c in $t.ChapterList) {
            $len = & $secs $c.Duration
            $res.Chapters += @{ Start = $at; Len = $len; Name = [string]$c.Name }
            $at += $len
        }
    } catch { Write-Log "Couldn't scan chapters of ${path}: $($_.Exception.Message)" }
    return $res
}

function Parse-Time([string]$s) {
    # 23.40 is a typo for 23:40 here; nobody splits on fractions of a second.
    $parts = $s.Trim().Replace('.', ':').Split(':')
    $v = 0.0
    foreach ($x in $parts) { $v = $v * 60 + [double]::Parse($x, [Globalization.CultureInfo]::InvariantCulture) }
    return $v
}

function Show-SplitDialog {
    $it = Selected-Item
    if (-not $it) { return }
    $src = $it.Src
    $Form.Cursor = [Windows.Forms.Cursors]::WaitCursor
    $scan = Scan-Chapters $src
    $Form.Cursor = [Windows.Forms.Cursors]::Default
    $chs = @($scan.Chapters)

    $d = New-Object Windows.Forms.Form
    $d.Text = 'Split into episodes'; $d.ClientSize = New-Object Drawing.Size(400, 404)
    $d.FormBorderStyle = 'FixedDialog'; $d.MaximizeBox = $false; $d.MinimizeBox = $false
    $d.StartPosition = 'CenterParent'; $d.BackColor = $Gray; $d.Font = $Font
    $d.Controls.Add((New-Label (Split-Path $src -Leaf) 10 8 380 $Bold))
    $what = New-Label '' 10 26 380; $d.Controls.Add($what)
    $list = New-Object Windows.Forms.CheckedListBox
    $list.Location = New-Object Drawing.Point(10, 46); $list.Size = New-Object Drawing.Size(380, 130)
    $list.CheckOnClick = $true; $list.IntegralHeight = $false
    $d.Controls.Add($list)
    foreach ($c in $chs) {
        $nm = $c.Name; if (-not $nm) { $nm = '' }
        [void]$list.Items.Add(("{0,-9} ({1})  {2}" -f (Format-Time $c.Start), (Format-Time $c.Len), $nm))
    }
    if ($chs.Count) { $what.Text = "Tick each chapter where an episode starts. $($chs.Count) chapters, $(Format-Time $scan.Duration) in all." }
    else { $what.Text = 'No chapters in this file. Type where each episode starts below.'; $list.Enabled = $false }

    $d.Controls.Add((New-Label 'Episodes are about' 10 184 104))
    $mins = New-Object Windows.Forms.NumericUpDown
    $mins.Location = New-Object Drawing.Point(116, 182); $mins.Size = New-Object Drawing.Size(44, 21); $mins.Minimum = 1; $mins.Maximum = 180; $mins.Value = 24
    $d.Controls.Add($mins)
    $d.Controls.Add((New-Label 'min long.' 164 184 56))
    $guess = New-Button 'Guess' 222 181 60 21; $d.Controls.Add($guess)

    $d.Controls.Add((New-Label 'Episodes start at (h:mm:ss, comma between):' 10 212 380 $Bold))
    $times = New-Object Windows.Forms.TextBox
    $times.Location = New-Object Drawing.Point(10, 228); $times.Size = New-Object Drawing.Size(380, 21)
    $d.Controls.Add($times)
    $preview = New-Object Windows.Forms.Label
    $preview.Location = New-Object Drawing.Point(10, 254); $preview.Size = New-Object Drawing.Size(380, 44); $preview.ForeColor = $Dim
    $d.Controls.Add($preview)
    $total = $scan.Duration; if ($total -le 0) { $total = Get-Duration $it }
    $d.Controls.Add((New-Label 'Show:' 10 302 40))
    $show = New-Object Windows.Forms.TextBox
    $show.Location = New-Object Drawing.Point(52, 300); $show.Size = New-Object Drawing.Size(170, 21)
    $show.Text = $(if ($it.Show.Trim()) { $it.Show.Trim() } else { $it.Title.Trim() })
    $d.Controls.Add($show)
    $d.Controls.Add((New-Label 'Season:' 230 302 46))
    $season = New-Object Windows.Forms.NumericUpDown
    $season.Location = New-Object Drawing.Point(276, 300); $season.Size = New-Object Drawing.Size(40, 21); $season.Minimum = 1; $season.Maximum = 99
    $d.Controls.Add($season)
    $d.Controls.Add((New-Label 'First ep:' 322 302 46))
    $first = New-Object Windows.Forms.NumericUpDown
    $first.Location = New-Object Drawing.Point(322, 318); $first.Size = New-Object Drawing.Size(48, 21); $first.Minimum = 1; $first.Maximum = 999
    $d.Controls.Add($first)
    $titles = New-Object Windows.Forms.CheckBox
    $titles.Text = 'Get episode titles from TVmaze'; $titles.Location = New-Object Drawing.Point(10, 334); $titles.AutoSize = $true; $titles.Checked = $true
    $d.Controls.Add($titles)
    $ok = New-Button 'Split' 234 370; $ok.DialogResult = 'OK'; $d.Controls.Add($ok)
    $no = New-Button 'Cancel' 315 370; $no.DialogResult = 'Cancel'; $d.Controls.Add($no); $d.CancelButton = $no

    # Ticks write the start times; typing your own switches to times.
    $st = @{ FromTicks = $false; Busy = $false }
    $list.Add_ItemCheck({
        param($s, $e)
        # Fires before the tick changes, so work out the new set here.
        $on = @($list.CheckedIndices | ForEach-Object { [int]$_ } | Where-Object { $_ -ne $e.Index })
        if ($e.NewValue -eq 'Checked') { $on += $e.Index }
        $st.Busy = $true
        $times.Text = (@($on | Sort-Object | ForEach-Object { Format-Time $chs[$_].Start }) -join ', ')
        $st.FromTicks = $true; $st.Busy = $false
    })
    $times.Add_TextChanged({
        if (-not $st.Busy) { $st.FromTicks = $false }
        # What you'll get, so a typo shows before anything is cut.
        try {
            $ts = @($times.Text.Split(',;'.ToCharArray()) | Where-Object { $_.Trim() } | ForEach-Object { Parse-Time $_ } | Sort-Object)
            $bits = @()
            for ($k = 0; $k -lt $ts.Count; $k++) {
                $end = $(if ($k + 1 -lt $ts.Count) { $ts[$k + 1] } else { $total })
                $bits += ('Ep {0}: {1}-{2} ({3})' -f ($k + 1), (Format-Time $ts[$k]), (Format-Time $end), (Format-Time ($end - $ts[$k])))
            }
            $preview.Text = $bits -join '    '
        } catch { $preview.Text = "Can't read those times. Use h:mm:ss, like 0:00, 23:40, 47:10." }
    })
    $guess.Add_Click({
        $len = [double]$mins.Value * 60
        if ($chs.Count) {
            # The chapter starting nearest each multiple of the episode length.
            for ($i = 0; $i -lt $chs.Count; $i++) { $list.SetItemChecked($i, $false) }
            $list.SetItemChecked(0, $true)
            $last = 0
            for ($t = $len; $t -lt $scan.Duration - $len / 3; $t += $len) {
                $best = 0
                for ($i = 0; $i -lt $chs.Count; $i++) { if ([Math]::Abs($chs[$i].Start - $t) -lt [Math]::Abs($chs[$best].Start - $t)) { $best = $i } }
                # A short chapter at the seam is the last episode's preview or tag:
                # the next episode starts after it.
                if ($chs[$best].Len -lt 60 -and $best + 1 -lt $chs.Count -and [Math]::Abs($chs[$best + 1].Start - $t) -lt 180) { $best++ }
                if ($best -gt $last) {
                    $list.SetItemChecked($best, $true)
                    $t = $chs[$best].Start    # count on from where the episode really started
                    $last = $best
                }
            }
        } else {
            $total = Get-Duration $it
            $ts = @(); for ($t = 0.0; $t -lt $total - $len / 3 -or $ts.Count -eq 0; $t += $len) { $ts += Format-Time $t; if ($total -le 0) { break } }
            $times.Text = $ts -join ', '
        }
    })
    if ($chs.Count) { $list.SetItemChecked(0, $true) }

    if ($d.ShowDialog($Form) -ne 'OK') { return }
    $parts = @()
    if ($st.FromTicks -and $list.CheckedIndices.Count) {
        $starts = @($list.CheckedIndices | ForEach-Object { [int]$_ })
        for ($k = 0; $k -lt $starts.Count; $k++) {
            $a = $starts[$k]; $b = $(if ($k + 1 -lt $starts.Count) { $starts[$k + 1] - 1 } else { $chs.Count - 1 })
            $len = 0.0; for ($i = $a; $i -le $b; $i++) { $len += $chs[$i].Len }
            $parts += @{ Chapters = "$($a + 1)-$($b + 1)"; Start = $chs[$a].Start; Len = $len }
        }
    } else {
        try { $ts = @($times.Text.Split(',;'.ToCharArray()) | Where-Object { $_.Trim() } | ForEach-Object { Parse-Time $_ } | Sort-Object) }
        catch { [Windows.Forms.MessageBox]::Show("Those times can't be read. Use h:mm:ss, like 0:00, 23:40, 47:10.", 'Boro Video Wizard', 'OK', 'Warning') | Out-Null; return }
        for ($k = 0; $k -lt $ts.Count; $k++) {
            $len = $(if ($k + 1 -lt $ts.Count) { $ts[$k + 1] - $ts[$k] } else { 0.0 })   # 0: to the end
            $parts += @{ Chapters = ''; Start = [double]$ts[$k]; Len = [double]$len }
        }
    }
    if ($parts.Count -lt 2) { [Windows.Forms.MessageBox]::Show('Pick at least two episode starts.', 'Boro Video Wizard', 'OK', 'Information') | Out-Null; return }

    $name = $show.Text.Trim()
    $map = $null
    if ($titles.Checked -and $name) { $Form.Cursor = [Windows.Forms.Cursors]::WaitCursor; $map = Find-Episodes $name; $Form.Cursor = [Windows.Forms.Cursors]::Default }
    $new = @()
    for ($k = 0; $k -lt $parts.Count; $k++) {
        $x = New-Item-For $src
        $x.Shelf = 1; $x.Show = $name; $x.Season = [int]$season.Value; $x.Episode = [int]$first.Value + $k; $x.EpGuessed = $true
        $x.Title = ''
        $key = "$($x.Season)x$($x.Episode)"
        if ($map -and $map.ContainsKey($key) -and $map[$key]) { $x.Title = Safe-Name $map[$key] }
        $parts[$k].N = $k + 1
        $x.Part = $parts[$k]
        $new += $x
    }
    # Swap the one row for its parts, in place.
    $at = $Items.IndexOf($it)
    $Items.RemoveAt($at)
    for ($k = $new.Count - 1; $k -ge 0; $k--) { $Items.Insert($at, $new[$k]) }
    $All[$src] = $new
    Fill-Table
}
$SplitBtn.Add_Click({ Show-SplitDialog })
foreach ($r in $Radios) { $r.Add_CheckedChanged({ if ($this.Checked) { Save-Editor 'shelf' } }) }
$ShowBox.Add_TextChanged({ Save-Editor 'show' }); $ShowBox.Add_SelectedIndexChanged({ Save-Editor 'show' })
$SeasonBox.Add_ValueChanged({ Save-Editor 'season' }); $EpBox.Add_ValueChanged({ Save-Editor 'episode' })
$TitleBox.Add_TextChanged({ Save-Editor 'title' }); $YearBox.Add_TextChanged({ Save-Editor 'year' })

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
    $dlg.Filter = 'Pictures|*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.webp;*.avif;*.heic|All files|*.*'
    if ($script:Folder) { $dlg.InitialDirectory = $script:Folder }
    if ($dlg.ShowDialog() -ne 'OK') { return $null }
    try { return Load-Image $dlg.FileName } catch {
        [Windows.Forms.MessageBox]::Show("That picture can't be read. Open it in Paint, Save as JPEG, and try that copy.", 'Boro Video Wizard', 'OK', 'Warning') | Out-Null
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
    if ($it.Shelf -eq 1) { $where = (Split-Path (Target-Path $it)).Substring($script:OutRoot.Length + 1) }
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
    if ($it.Part -and $it.Part.Chapters) { $hb += @('-c', $it.Part.Chapters) }
    elseif ($it.Part) {
        $inv = [Globalization.CultureInfo]::InvariantCulture
        $hb += @('--start-at', ('seconds:' + $it.Part.Start.ToString('0.##', $inv)))
        if ($it.Part.Len -gt 0) { $hb += @('--stop-at', ('seconds:' + $it.Part.Len.ToString('0.##', $inv))) }
    }
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
    Write-Log "Converting $($it.Src) with: $($hb -join ' ')"
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
    # Posters never looked at (Frames for the rest) are grabbed now.
    if ($it.Poster -eq 'frame' -and -not $it.NoFrames -and -not $it.Frames.ContainsKey($it.FramePage)) { Get-Frames $it $it.FramePage }
    $img = Poster-Image $it
    if ($img) {
        try { Save-Jpeg $img ([IO.Path]::ChangeExtension($dest, '.jpg')) 480 }
        catch { Write-Log "Couldn't save the poster for ${dest}: $($_.Exception.Message)" }
    }
    if ($it.Shelf -eq 1) {
        $cv = Cover-For $it.Show
        if (-not $cv.Asked -and -not (Has-Cover $it.Show)) { $cv.Asked = $true; $cv.Img = Find-Cover $it.Show.Trim() }
        $coverPath = Join-Path (Split-Path (Split-Path $dest)) 'cover.jpg'
        if ($cv.Img -and -not (Has-Cover $it.Show)) {
            try { Save-Jpeg $cv.Img $coverPath 400 } catch { Write-Log "Couldn't save $coverPath" }
        }
    }
    # A split file stays put until its last part is done.
    $others = @($Items | Where-Object { $_.Src -eq $it.Src -and -not [object]::ReferenceEquals($_, $it) -and
        $_.Status -ne 'done' -and $_.Status -ne 'failed' })
    if ($others.Count) { Write-Log "Filed $dest"; return }
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
    $RestBtn.Visible = $script:Step -eq 3 -and $script:PosterAt -lt $Items.Count - 1
    $Form.AcceptButton = $null
}
# Skips the poster pages left: each of those videos gets a frame from about 10% in.
$RestBtn.Add_Click({ Go 4; Start-Brew; Update-Nav })

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
        foreach ($x in @($All[$path])) { [void]$Items.Add($x) }   # a split file is several items
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
