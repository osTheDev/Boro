# Boro Video Wizard
#
# Watches a folder (Downloads unless you pick another with "Folder..."). Each new video is converted with HandBrakeCLI
# to an MP4 the PSP plays (H.264 Main, at most 480x272, stereo AAC), then you
# name it and pick its shelf, and it lands in <watched folder>\PSP\VIDEO laid
# out the way Boro reads it:
#
#   VIDEO\Movies\<Title>.mp4
#   VIDEO\Shows\<Show>\Season N\<Show> S01E03 <Episode title>.mp4  (+ cover.jpg)
#   VIDEO\Music\<Artist - Song>.mp4
#   VIDEO\Clips\<Title>.mp4
#
# The original moves to <watched folder>\Originals. Copy the PSP\VIDEO folder
# onto the memory stick (merging folders) to put the videos on the PSP.
#
# Start it with Start-BoroWizard.bat. It keeps running until you press Quit.

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# ---- Settings ---------------------------------------------------------------

$SettingsFile = Join-Path $PSScriptRoot 'BoroWizard.settings.txt'
$WorkDir   = Join-Path $env:TEMP 'BoroWizard'

# The watched folder, and what lives under it.
function Set-Watch([string]$dir) {
    $script:Watch     = $dir
    $script:OutRoot   = Join-Path $dir 'PSP\VIDEO'
    $script:Originals = Join-Path $dir 'Originals'
    $script:LogFile   = Join-Path $dir 'PSP\BoroWizard.log'
    foreach ($d in @($script:OutRoot, $script:Originals)) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
}

$Watch = Join-Path $env:USERPROFILE 'Downloads'
if (Test-Path -LiteralPath $SettingsFile) {
    $line = Get-Content -LiteralPath $SettingsFile | Where-Object { $_ -like 'Watch=*' } | Select-Object -First 1
    if ($line -and (Test-Path -LiteralPath $line.Substring(6))) { $Watch = $line.Substring(6) }
}
$VideoExts = @('.mp4', '.mkv', '.avi', '.mov', '.m4v', '.wmv', '.flv', '.webm', '.mpg', '.mpeg', '.ts')
$AutoFileSeconds = 120   # an unanswered naming page files the video as shown

$Shelves = @(
    @{ Name = 'Movie';       Folder = 'Movies' },
    @{ Name = 'TV episode';  Folder = 'Shows' },
    @{ Name = 'Music video'; Folder = 'Music' },
    @{ Name = 'Clip';        Folder = 'Clips' }
)

function Find-HandBrake {
    $here = Join-Path $PSScriptRoot 'HandBrakeCLI.exe'
    if (Test-Path $here) { return $here }
    $cmd = Get-Command 'HandBrakeCLI.exe' -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($p in @("$env:ProgramFiles\HandBrake\HandBrakeCLI.exe", "${env:ProgramFiles(x86)}\HandBrake\HandBrakeCLI.exe",
                     "$env:ProgramFiles\HandBrakeCLI\HandBrakeCLI.exe")) {
        if ($p -and (Test-Path $p)) { return $p }
    }
    return $null
}

$HandBrake = Find-HandBrake
if (-not $HandBrake) {
    [Windows.Forms.MessageBox]::Show(
        "HandBrakeCLI.exe wasn't found.`n`nDownload the command line version from handbrake.fr (Downloads > Command Line Version), " +
        "then put HandBrakeCLI.exe in this folder:`n`n$PSScriptRoot",
        'Boro Video Wizard', 'OK', 'Warning') | Out-Null
    exit 1
}

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
Set-Watch $Watch

function Write-Log([string]$msg) {
    $line = '{0:yyyy-MM-dd HH:mm:ss}  {1}' -f (Get-Date), $msg
    Add-Content -Path $LogFile -Value $line -ErrorAction SilentlyContinue
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
    $s = $s.Trim(' ', '.')
    if (-not $s) { $s = 'Video' }
    return $s
}

# A guess at what a file is, from its name.
function Guess-Video([string]$fileName) {
    $base = [IO.Path]::GetFileNameWithoutExtension($fileName)
    $g = @{ Shelf = 0; Title = ''; Show = ''; Season = 1; Episode = 1 }
    if ($base -match '^(.*?)[\s._\-]*[Ss](\d{1,2})[\s._]?[Ee](\d{1,3})(.*)$' -or
        $base -match '^(.*?)[\s._\-]+(\d{1,2})x(\d{2,3})(.*)$') {
        $g.Shelf = 1
        $g.Show = Clean-Name ($Matches[1] -replace '[\s._\-]*\(?(19|20)\d{2}\)?$', '')
        $g.Season = [int]$Matches[2]
        $g.Episode = [int]$Matches[3]
        $g.Title = Clean-Name $Matches[4]
        return $g
    }
    $t = Clean-Name $base
    $t = ($t -replace '\s*\(?\b(19|20)\d{2}\b\)?\s*$', '').Trim()
    if (-not $t) { $t = Clean-Name $base }
    if ($base -match ' - ' -and $base -notmatch '(?i)\b(19|20)\d{2}\b') { $g.Shelf = 2 }
    $g.Title = $t
    return $g
}

function Existing-Shows {
    $dir = Join-Path $OutRoot 'Shows'
    if (-not (Test-Path $dir)) { return @() }
    return @(Get-ChildItem -Path $dir -Directory | Sort-Object Name | ForEach-Object { $_.Name })
}

function Unique-Path([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $path }
    $dir = Split-Path $path; $stem = [IO.Path]::GetFileNameWithoutExtension($path); $ext = [IO.Path]::GetExtension($path)
    for ($i = 2; ; $i++) {
        $p = Join-Path $dir ("{0} ({1}){2}" -f $stem, $i, $ext)
        if (-not (Test-Path -LiteralPath $p)) { return $p }
    }
}

# ---- Covers -------------------------------------------------------------------

function Load-Image([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $ms = New-Object IO.MemoryStream(, $bytes)
    return [Drawing.Image]::FromStream($ms)
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

# Saves a cover as a JPEG about 400 pixels tall: plenty for Boro's posters.
function Save-Cover([Drawing.Image]$img, [string]$path) {
    $h = [Math]::Min(400, $img.Height)
    $w = [int]($img.Width * $h / $img.Height)
    $bmp = New-Object Drawing.Bitmap($w, $h)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.DrawImage($img, 0, 0, $w, $h)
    $g.Dispose()
    $bmp.Save($path, [Drawing.Imaging.ImageFormat]::Jpeg)
    $bmp.Dispose()
}

# ---- Converting ---------------------------------------------------------------

$Queue   = New-Object System.Collections.ArrayList   # paths waiting to convert
$Naming  = New-Object System.Collections.ArrayList   # converted, waiting for a name
$Seen    = @{}                                       # path -> size last poll, or 'busy' / 'done' / 'failed'
$Job     = $null                                     # the conversion running
$Paused  = $false

function Start-Next {
    if ($script:Job -or $script:Paused -or $Queue.Count -eq 0) { return }
    $src = $Queue[0]; $Queue.RemoveAt(0)
    if (-not (Test-Path -LiteralPath $src)) { return }
    $id = [guid]::NewGuid().ToString('N').Substring(0, 8)
    $out = Join-Path $WorkDir "$id.mp4"
    $json = Join-Path $WorkDir "$id.out.txt"
    $err = Join-Path $WorkDir "$id.err.txt"
    $hb = @(
        '-i', "`"$src`"", '-o', "`"$out`"", '--json',
        '-f', 'av_mp4', '--optimize',
        '-e', 'x264', '--encoder-preset', 'medium', '--encoder-profile', 'main', '--encoder-level', '3.0', '-q', '22',
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
    $script:Job = @{ Src = $src; Out = $out; Json = $json; Err = $err; Proc = $p; Started = Get-Date; Progress = 0.0; Eta = -1 }
    Write-Log "Converting $src"
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

function Finish-Job {
    $j = $script:Job
    $script:Job = $null
    $ok = $j.Proc.ExitCode -eq 0 -and (Test-Path -LiteralPath $j.Out) -and (Get-Item -LiteralPath $j.Out).Length -gt 0
    if ($ok) {
        $Seen[$j.Src] = 'done'
        [void]$Naming.Add(@{ Src = $j.Src; Out = $j.Out; Guess = (Guess-Video (Split-Path $j.Src -Leaf)) })
        Write-Log "Converted $($j.Src)"
    } else {
        $Seen[$j.Src] = 'failed'
        $why = ''
        if (Test-Path $j.Err) { $why = (Get-Content $j.Err -Tail 3 -ErrorAction SilentlyContinue) -join ' ' }
        Write-Log "FAILED $($j.Src) (exit $($j.Proc.ExitCode)) $why"
        if (Test-Path $j.Out) { Remove-Item $j.Out -Force -ErrorAction SilentlyContinue }
        [void]$Done.Items.Insert(0, "Couldn't convert $(Split-Path $j.Src -Leaf) (see BoroWizard.log)")
    }
    Remove-Item $j.Json, $j.Err -Force -ErrorAction SilentlyContinue
}

# New videos in Downloads, once they've stopped growing and nothing has them open.
function Scan-Downloads {
    foreach ($f in Get-ChildItem -LiteralPath $Watch -File -ErrorAction SilentlyContinue) {
        if ($VideoExts -notcontains $f.Extension.ToLower()) { continue }
        $k = $f.FullName
        $was = $Seen[$k]
        if ($was -is [string]) { continue }
        if ($null -eq $was -or $was -ne $f.Length) { $Seen[$k] = $f.Length; continue }
        try { $fs = [IO.File]::Open($k, 'Open', 'Read', 'None'); $fs.Close() } catch { continue }
        $Seen[$k] = 'busy'
        [void]$Queue.Add($k)
    }
}

# ---- Window -----------------------------------------------------------------------

$Gray = [Drawing.Color]::FromArgb(192, 192, 192)
$Navy = [Drawing.Color]::FromArgb(0, 0, 128)
$Font = New-Object Drawing.Font('Microsoft Sans Serif', 8.25)
$Bold = New-Object Drawing.Font('Microsoft Sans Serif', 8.25, [Drawing.FontStyle]::Bold)
$Head = New-Object Drawing.Font('Microsoft Sans Serif', 12, [Drawing.FontStyle]::Bold)

function New-Label($text, $x, $y, $w = 360, $font = $Font) {
    $l = New-Object Windows.Forms.Label
    $l.Text = $text; $l.Location = New-Object Drawing.Point($x, $y); $l.Size = New-Object Drawing.Size($w, 18); $l.Font = $font
    return $l
}
function New-Button($text, $x, $y, $w = 75) {
    $b = New-Object Windows.Forms.Button
    $b.Text = $text; $b.Location = New-Object Drawing.Point($x, $y); $b.Size = New-Object Drawing.Size($w, 23)
    $b.FlatStyle = 'Standard'; $b.BackColor = $Gray; $b.Font = $Font
    return $b
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

# ---- Page 1: working ----
$Work = New-Object Windows.Forms.Panel
$Work.Location = New-Object Drawing.Point(146, 8); $Work.Size = New-Object Drawing.Size(366, 306)
$Form.Controls.Add($Work)
$Work.Controls.Add((New-Label 'Brewing videos for your PSP...' 0 4 366 $Head))
$WatchLabel = New-Label '' 0 30 366; $Work.Controls.Add($WatchLabel)
$Work.Controls.Add((New-Label "Drop a video in and I'll shrink it down to PSP size." 0 46))
function Show-Watch {
    $name = Split-Path $script:Watch -Leaf
    $WatchLabel.Text = "I'm watching your $name folder."
}
Show-Watch
$Work.Controls.Add((New-Label 'Now converting:' 0 72 366 $Bold))
$NowName = New-Label '(nothing yet)' 0 88; $Work.Controls.Add($NowName)
$Bar = New-Object Windows.Forms.ProgressBar
$Bar.Location = New-Object Drawing.Point(0, 106); $Bar.Size = New-Object Drawing.Size(360, 18); $Bar.Style = 'Blocks'; $Bar.Maximum = 1000
$Work.Controls.Add($Bar)
$NowInfo = New-Label '' 0 126; $NowInfo.ForeColor = [Drawing.Color]::FromArgb(60, 60, 60); $Work.Controls.Add($NowInfo)
$Work.Controls.Add((New-Label 'Waiting in line:' 0 150 366 $Bold))
$Waiting = New-Object Windows.Forms.ListBox
$Waiting.Location = New-Object Drawing.Point(0, 166); $Waiting.Size = New-Object Drawing.Size(360, 56); $Waiting.IntegralHeight = $false
$Work.Controls.Add($Waiting)
$Work.Controls.Add((New-Label 'Done today:' 0 228 366 $Bold))
$Done = New-Object Windows.Forms.ListBox
$Done.Location = New-Object Drawing.Point(0, 244); $Done.Size = New-Object Drawing.Size(360, 60); $Done.IntegralHeight = $false
$Work.Controls.Add($Done)

# ---- Page 2: naming ----
$Name = New-Object Windows.Forms.Panel
$Name.Location = New-Object Drawing.Point(146, 8); $Name.Size = New-Object Drawing.Size(366, 306); $Name.Visible = $false
$Form.Controls.Add($Name)
$Name.Controls.Add((New-Label 'Poof! A new video is ready.' 0 4 366 $Head))
$SrcLabel = New-Label '' 0 28 366; $SrcLabel.ForeColor = [Drawing.Color]::FromArgb(60, 60, 60); $Name.Controls.Add($SrcLabel)
$Name.Controls.Add((New-Label 'Shelf:' 0 50 366 $Bold))
$Radios = @()
$rx = @(0, 62, 152, 252)
for ($i = 0; $i -lt 4; $i++) {
    $r = New-Object Windows.Forms.RadioButton
    $r.Text = $Shelves[$i].Name; $r.Location = New-Object Drawing.Point($rx[$i], 66); $r.AutoSize = $true; $r.Tag = $i
    $Name.Controls.Add($r); $Radios += $r
}
$ShowLabel = New-Label 'Show:' 0 92 200 $Bold; $Name.Controls.Add($ShowLabel)
$ShowBox = New-Object Windows.Forms.ComboBox
$ShowBox.Location = New-Object Drawing.Point(0, 108); $ShowBox.Size = New-Object Drawing.Size(196, 21); $ShowBox.DropDownStyle = 'DropDown'
$Name.Controls.Add($ShowBox)
$SeasonLabel = New-Label 'Season:' 206 92 70 $Bold; $Name.Controls.Add($SeasonLabel)
$SeasonBox = New-Object Windows.Forms.NumericUpDown
$SeasonBox.Location = New-Object Drawing.Point(206, 108); $SeasonBox.Size = New-Object Drawing.Size(60, 21); $SeasonBox.Minimum = 1; $SeasonBox.Maximum = 99
$Name.Controls.Add($SeasonBox)
$EpLabel = New-Label 'Episode:' 276 92 80 $Bold; $Name.Controls.Add($EpLabel)
$EpBox = New-Object Windows.Forms.NumericUpDown
$EpBox.Location = New-Object Drawing.Point(276, 108); $EpBox.Size = New-Object Drawing.Size(84, 21); $EpBox.Minimum = 1; $EpBox.Maximum = 999
$Name.Controls.Add($EpBox)
$TitleLabel = New-Label 'Title:' 0 136 366 $Bold; $Name.Controls.Add($TitleLabel)
$TitleBox = New-Object Windows.Forms.TextBox
$TitleBox.Location = New-Object Drawing.Point(0, 152); $TitleBox.Size = New-Object Drawing.Size(360, 21)
$Name.Controls.Add($TitleBox)
$CoverLabel = New-Label 'Cover (new show):' 0 180 366 $Bold; $Name.Controls.Add($CoverLabel)
$CoverPic = New-Object Windows.Forms.PictureBox
$CoverPic.Location = New-Object Drawing.Point(0, 196); $CoverPic.Size = New-Object Drawing.Size(40, 58)
$CoverPic.BorderStyle = 'Fixed3D'; $CoverPic.SizeMode = 'Zoom'; $CoverPic.BackColor = [Drawing.Color]::White
$Name.Controls.Add($CoverPic)
$CoverNote = New-Label '' 50 198 310; $Name.Controls.Add($CoverNote)
$FindBtn = New-Button 'Find again' 50 222 80; $Name.Controls.Add($FindBtn)
$PickBtn = New-Button 'Pick my own...' 136 222 100; $Name.Controls.Add($PickBtn)
$NoCoverBtn = New-Button 'No cover' 242 222 70; $Name.Controls.Add($NoCoverBtn)
$SavesTo = New-Label '' 0 262 366; $SavesTo.Size = New-Object Drawing.Size(366, 30); $Name.Controls.Add($SavesTo)
$Countdown = New-Label '' 0 290 366; $Countdown.ForeColor = [Drawing.Color]::FromArgb(60, 60, 60); $Name.Controls.Add($Countdown)

# Buttons along the bottom.
$FolderBtn = New-Button 'Folder...' 275 330; $Form.Controls.Add($FolderBtn)
$PauseBtn = New-Button 'Pause' 356 330; $Form.Controls.Add($PauseBtn)
$QuitBtn = New-Button 'Quit' 437 330; $Form.Controls.Add($QuitBtn)
$FinishBtn = New-Button 'Finish' 356 330; $FinishBtn.Visible = $false; $Form.Controls.Add($FinishBtn)
$SkipBtn = New-Button 'Skip' 437 330; $SkipBtn.Visible = $false; $Form.Controls.Add($SkipBtn)
$Form.AcceptButton = $null

# ---- Naming page logic ----
$Current = $null      # the video being named
$Cover = $null        # [Drawing.Image] for a new show, or $null
$CoverFor = ''        # the show name it was found for
$Deadline = $null     # when the page files the video by itself; $null once touched

function Shelf { for ($i = 0; $i -lt 4; $i++) { if ($Radios[$i].Checked) { return $i } }; return 0 }
function Is-NewShow { $s = $ShowBox.Text.Trim(); return $s -and ((Existing-Shows) -notcontains $s) }

function Target-Path {
    $title = Safe-Name $TitleBox.Text
    $shelf = Shelf
    $folder = Join-Path $OutRoot $Shelves[$shelf].Folder
    if ($shelf -eq 1) {
        $show = Safe-Name $ShowBox.Text
        $file = '{0} S{1:D2}E{2:D2}' -f $show, [int]$SeasonBox.Value, [int]$EpBox.Value
        if ($TitleBox.Text.Trim()) { $file += " $title" }
        return Join-Path (Join-Path (Join-Path $folder $show) ("Season {0}" -f [int]$SeasonBox.Value)) "$file.mp4"
    }
    return Join-Path $folder "$title.mp4"
}

function Update-Naming {
    $tv = (Shelf) -eq 1
    foreach ($c in @($ShowLabel, $ShowBox, $SeasonLabel, $SeasonBox, $EpLabel, $EpBox)) { $c.Visible = $tv }
    if ($tv) { $TitleLabel.Text = 'Episode title:' } elseif ((Shelf) -eq 2) { $TitleLabel.Text = 'Artist - Song:' } else { $TitleLabel.Text = 'Title:' }
    $newShow = $tv -and (Is-NewShow)
    foreach ($c in @($CoverLabel, $CoverPic, $CoverNote, $FindBtn, $PickBtn, $NoCoverBtn)) { $c.Visible = $newShow }
    $rel = (Target-Path).Substring($OutRoot.Length + 1)
    $extra = ''
    if ($newShow -and $script:Cover) { $extra = '  +  cover.jpg' }
    $SavesTo.Text = "Saves to:  PSP\VIDEO\$rel$extra"
}

function Look-For-Cover {
    $show = $ShowBox.Text.Trim()
    if (-not (Is-NewShow) -or $show -eq $script:CoverFor) { return }
    $script:CoverFor = $show
    $CoverNote.Text = 'Looking on TVmaze...'; $Form.Refresh()
    $script:Cover = Find-Cover $show
    $CoverPic.Image = $script:Cover
    if ($script:Cover) { $CoverNote.Text = 'Found on TVmaze' } else { $CoverNote.Text = 'No cover found. Pick your own?' }
    Update-Naming
}

function Show-Naming($item) {
    $script:Current = $item
    $g = $item.Guess
    $SrcLabel.Text = Split-Path $item.Src -Leaf
    $ShowBox.Items.Clear(); foreach ($s in Existing-Shows) { [void]$ShowBox.Items.Add($s) }
    # Match an existing show regardless of case.
    $known = Existing-Shows | Where-Object { $_ -ieq $g.Show } | Select-Object -First 1
    if ($known) { $ShowBox.Text = $known } else { $ShowBox.Text = $g.Show }
    $script:Cover = $null; $script:CoverFor = ''; $CoverPic.Image = $null; $CoverNote.Text = ''
    $SeasonBox.Value = [Math]::Max(1, [Math]::Min(99, $g.Season))
    $EpBox.Value = [Math]::Max(1, [Math]::Min(999, $g.Episode))
    $TitleBox.Text = $g.Title
    $Radios[$g.Shelf].Checked = $true
    $script:Deadline = (Get-Date).AddSeconds($AutoFileSeconds)
    $Work.Visible = $false; $PauseBtn.Visible = $false; $QuitBtn.Visible = $false; $FolderBtn.Visible = $false
    $Name.Visible = $true; $FinishBtn.Visible = $true; $SkipBtn.Visible = $true
    $Form.Text = 'Boro Video Wizard - New video ready'
    Update-Naming
    if ((Shelf) -eq 1) { Look-For-Cover }
    $TitleBox.Focus() | Out-Null
}

function Hide-Naming {
    $script:Current = $null
    $Name.Visible = $false; $FinishBtn.Visible = $false; $SkipBtn.Visible = $false
    $Work.Visible = $true; $PauseBtn.Visible = $true; $QuitBtn.Visible = $true; $FolderBtn.Visible = $true
    $Form.Text = 'Boro Video Wizard'
}

function File-Current {
    $item = $script:Current
    if (-not $item) { return }
    $dest = Unique-Path (Target-Path)
    try {
        New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
        if ((Shelf) -eq 1 -and $script:Cover) {
            $coverPath = Join-Path (Split-Path (Split-Path $dest)) 'cover.jpg'
            if (-not (Test-Path -LiteralPath $coverPath)) { Save-Cover $script:Cover $coverPath }
        }
        Move-Item -LiteralPath $item.Out -Destination $dest
        if (Test-Path -LiteralPath $item.Src) {
            Move-Item -LiteralPath $item.Src -Destination (Unique-Path (Join-Path $Originals (Split-Path $item.Src -Leaf)))
        }
        $rel = $dest.Substring($OutRoot.Length + 1)
        [void]$Done.Items.Insert(0, ("{0}  ->  VIDEO\{1}" -f [IO.Path]::GetFileNameWithoutExtension($dest), (Split-Path $rel)))
        Write-Log "Filed $dest"
    } catch {
        Write-Log "Couldn't file $($item.Src): $($_.Exception.Message)"
        [Windows.Forms.MessageBox]::Show("Couldn't save it there:`n$($_.Exception.Message)", 'Boro Video Wizard', 'OK', 'Warning') | Out-Null
        return
    }
    $Naming.Remove($item)
    Hide-Naming
}

# Anything you do on the page stops the countdown.
$Touch = { $script:Deadline = $null; $Countdown.Text = '' }
foreach ($r in $Radios) { $r.Add_CheckedChanged({ Update-Naming; if ((Shelf) -eq 1) { Look-For-Cover } }); $r.Add_Click($Touch) }
$ShowBox.Add_TextChanged({ Update-Naming }); $ShowBox.Add_KeyDown($Touch); $ShowBox.Add_Leave({ Look-For-Cover })
$ShowBox.Add_SelectedIndexChanged({ Update-Naming; $script:Deadline = $null })
$SeasonBox.Add_ValueChanged({ Update-Naming }); $SeasonBox.Add_Enter($Touch)
$EpBox.Add_ValueChanged({ Update-Naming }); $EpBox.Add_Enter($Touch)
$TitleBox.Add_TextChanged({ Update-Naming }); $TitleBox.Add_KeyDown($Touch)
$FindBtn.Add_Click({ & $Touch; $script:CoverFor = ''; Look-For-Cover })
$PickBtn.Add_Click({
    & $Touch
    $dlg = New-Object Windows.Forms.OpenFileDialog
    $dlg.Filter = 'Pictures|*.jpg;*.jpeg;*.png;*.bmp;*.webp|All files|*.*'
    $dlg.InitialDirectory = $Watch
    if ($dlg.ShowDialog() -eq 'OK') {
        try {
            $script:Cover = Load-Image $dlg.FileName; $CoverPic.Image = $script:Cover
            $CoverNote.Text = 'Your picture'; $script:CoverFor = $ShowBox.Text.Trim()
        } catch { $CoverNote.Text = "That picture can't be read." }
        Update-Naming
    }
})
$NoCoverBtn.Add_Click({ & $Touch; $script:Cover = $null; $CoverPic.Image = $null; $CoverNote.Text = 'No cover: Boro uses a thumbnail'; Update-Naming })
$FinishBtn.Add_Click({ File-Current })
$SkipBtn.Add_Click({ File-Current })   # files it as guessed, right away

$PauseBtn.Add_Click({
    $script:Paused = -not $script:Paused
    if ($script:Paused) { $PauseBtn.Text = 'Resume' } else { $PauseBtn.Text = 'Pause' }
})
$FolderBtn.Add_Click({
    $dlg = New-Object Windows.Forms.FolderBrowserDialog
    $dlg.Description = 'Pick the folder to watch. Converted videos go in PSP\VIDEO inside it, originals in Originals.'
    $dlg.SelectedPath = $script:Watch
    if ($dlg.ShowDialog() -ne 'OK' -or $dlg.SelectedPath -eq $script:Watch) { return }
    Set-Watch $dlg.SelectedPath
    Set-Content -LiteralPath $SettingsFile -Value ("Watch=" + $dlg.SelectedPath)
    # Forget what was waiting in the old folder; whatever is converting finishes.
    $Queue.Clear()
    foreach ($k in @($Seen.Keys)) { if ($Seen[$k] -isnot [string] -or $Seen[$k] -eq 'busy') { $Seen.Remove($k) } }
    if ($script:Job) { $Seen[$script:Job.Src] = 'busy' }
    Show-Watch
    Write-Log "Now watching $($dlg.SelectedPath)"
})
$QuitBtn.Add_Click({ $Form.Close() })
$Form.Add_FormClosing({
    param($s, $e)
    if ($script:Job -and -not $script:Job.Proc.HasExited) {
        $a = [Windows.Forms.MessageBox]::Show("A video is still converting. Quit anyway?`nIt'll start over next time.", 'Boro Video Wizard', 'YesNo', 'Question')
        if ($a -ne 'Yes') { $e.Cancel = $true; return }
        # cmd and the HandBrakeCLI under it.
        & taskkill.exe /PID $script:Job.Proc.Id /T /F 2>&1 | Out-Null
        Remove-Item $script:Job.Out -Force -ErrorAction SilentlyContinue
    }
})

# ---- The heartbeat --------------------------------------------------------------

$Tick = 0
$Timer = New-Object Windows.Forms.Timer
$Timer.Interval = 1000
$Timer.Add_Tick({
    $script:Tick++
    if ($script:Tick % 3 -eq 1) { Scan-Downloads }
    if ($script:Job) {
        if ($script:Job.Proc.HasExited) { Finish-Job } else { Read-Progress }
    }
    Start-Next

    if ($script:Job) {
        $NowName.Text = Split-Path $script:Job.Src -Leaf
        $pc = [Math]::Max(0, [Math]::Min(1, $script:Job.Progress))
        $Bar.Value = [int]($pc * 1000)
        $info = '{0:0}%' -f ($pc * 100)
        if ($script:Job.Eta -ge 0) { $info += '  -  about {0} min left' -f [Math]::Max(1, [Math]::Ceiling($script:Job.Eta / 60)) }
        $NowInfo.Text = $info
    } else {
        $Bar.Value = 0; $NowInfo.Text = ''
        if ($script:Paused) { $NowName.Text = '(paused)' } else { $NowName.Text = '(nothing yet)' }
    }
    $Waiting.BeginUpdate(); $Waiting.Items.Clear()
    foreach ($q in $Queue) { [void]$Waiting.Items.Add((Split-Path $q -Leaf)) }
    if ($Naming.Count -gt 1 -or ($Naming.Count -eq 1 -and -not $script:Current)) {
        [void]$Waiting.Items.Add(("{0} ready to name" -f $Naming.Count))
    }
    $Waiting.EndUpdate()

    if (-not $script:Current -and $Naming.Count -gt 0) { Show-Naming $Naming[0] }
    if ($script:Current -and $script:Deadline) {
        $left = [int]($script:Deadline - (Get-Date)).TotalSeconds
        if ($left -le 0) { File-Current } else { $Countdown.Text = "No answer in $left s? I'll file it as shown." }
    }
})

Write-Log "Started; watching $Watch with $HandBrake"
$Timer.Start()
[void]$Form.ShowDialog()
$Timer.Stop()
Write-Log 'Quit'
