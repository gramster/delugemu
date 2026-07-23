<#
.SYNOPSIS
    Native Windows launcher for the packaged Deluge emulator (delugemu).

.DESCRIPTION
    A PowerShell port of scripts/run.sh for the relocatable Windows bundle, so a
    clean Windows machine (no MSYS2) gets the full experience: optional firmware
    with auto-download of the community release, SD images and SD *folders*
    (snapshotted into a FAT image, with write-back for '_rw' folders) including
    auto-detection of a default sdcard folder and download of the Synthstrom
    factory card, MIDI routing over QEMU chardevs, audio backend selection and
    display modes.

    It is shipped alongside qemu-system-arm.exe in the Windows release bundle and
    invoked by delugemu.cmd. The macOS-only 'coremidi' MIDI shortcut is not
    available on Windows; use a chardev spec (e.g. udp:127.0.0.1:1999) instead.

.NOTES
    SPDX-License-Identifier: GPL-2.0-or-later
#>

[CmdletBinding()]
param(
    # Set by the Start Menu shortcut the MSI installs. The install folder may
    # not be writable and there is no visible console, so state moves to
    # %APPDATA%\DelugEmu and prompts/errors become message boxes.
    [switch] $AppMode,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $CliArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Bundle root = the directory this script lives in.
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path

$StateDir = $null
if ($AppMode) {
    Add-Type -AssemblyName System.Windows.Forms | Out-Null
    $StateDir = Join-Path $env:APPDATA 'DelugEmu'
    New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
    Set-Location $StateDir
}

function Write-Log  { param([string]$Msg) Write-Host "[delugemu] $Msg" -ForegroundColor Cyan }
function Write-Warn { param([string]$Msg) Write-Host "[delugemu] $Msg" -ForegroundColor Yellow }
function Die {
    param([string]$Msg)
    Hide-Splash
    if ($AppMode) {
        [void][System.Windows.Forms.MessageBox]::Show($Msg, 'DelugEmu',
            [System.Windows.Forms.MessageBoxButtons]::OK,
            [System.Windows.Forms.MessageBoxIcon]::Error)
    }
    Write-Host "[delugemu] $Msg" -ForegroundColor Red
    exit 1
}

# Yes/no question: console Read-Host normally, a message box in app mode (the
# hidden console cannot take input).
function Confirm-User {
    param([string]$Msg)
    if ($AppMode) {
        $r = [System.Windows.Forms.MessageBox]::Show($Msg, 'DelugEmu',
            [System.Windows.Forms.MessageBoxButtons]::YesNo,
            [System.Windows.Forms.MessageBoxIcon]::Question)
        return ($r -eq [System.Windows.Forms.DialogResult]::Yes)
    }
    $reply = Read-Host "$Msg [y/N]"
    return ($reply -match '^(y|yes)$')
}

# Non-blocking progress note. In app mode long steps (factory-card download,
# SD image build) would otherwise look like nothing is happening; a balloon
# tip tells the user the window opens when the work finishes.
$script:Tray = $null
function Notify {
    param([string]$Msg)
    Write-Log $Msg
    if (-not $AppMode) { return }
    try {
        if (-not $script:Tray) {
            Add-Type -AssemblyName System.Drawing | Out-Null
            $script:Tray = New-Object System.Windows.Forms.NotifyIcon
            $script:Tray.Icon = [System.Drawing.SystemIcons]::Information
            $script:Tray.Visible = $true
        }
        $script:Tray.ShowBalloonTip(10000, 'DelugEmu', $Msg,
            [System.Windows.Forms.ToolTipIcon]::Info)
    } catch { }
}

# Startup splash for app mode: a separate PowerShell process showing a marquee
# progress form, so the pre-boot work (downloads, SD image build) doesn't look
# like a hang. Killed right before the emulator window appears.
$script:SplashProc = $null
function Show-Splash {
    if (-not $AppMode) { return }
    $code = @'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$f = New-Object System.Windows.Forms.Form
$f.Text = 'DelugEmu'
$f.ClientSize = New-Object System.Drawing.Size(420, 110)
$f.StartPosition = 'CenterScreen'
$f.FormBorderStyle = 'FixedDialog'
$f.MaximizeBox = $false; $f.MinimizeBox = $false
$f.TopMost = $true
$p = New-Object System.Windows.Forms.ProgressBar
$p.Style = 'Marquee'; $p.SetBounds(20, 20, 380, 20)
$l = New-Object System.Windows.Forms.Label
$l.Text = "Starting DelugEmu...`nThe window opens when the emulator is ready. A first launch can take a few minutes (downloads and SD card build)."
$l.SetBounds(20, 50, 380, 55)
$f.Controls.Add($p); $f.Controls.Add($l)
[System.Windows.Forms.Application]::Run($f)
'@
    try {
        $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
        $script:SplashProc = Start-Process powershell -PassThru -WindowStyle Hidden `
            -ArgumentList @('-NoProfile', '-EncodedCommand', $enc)
    } catch { }
}
function Hide-Splash {
    if ($script:SplashProc) {
        try { Stop-Process -Id $script:SplashProc.Id -Force -ErrorAction SilentlyContinue } catch { }
        $script:SplashProc = $null
    }
}

# Splash up as early as possible in app mode (functions above are defined now).
if ($AppMode) { Show-Splash }

function Show-Usage {
    @'
Run Deluge firmware under the emulator (Windows).

Usage:
  delugemu.cmd [firmware.elf|firmware.bin] [options] [-- qemu args...]

The firmware image is optional. If omitted, the launcher uses a .bin/.elf from
the firmware folder (default: <bundle>\firmware; override with the
DELUGEMU_FIRMWARE_DIR environment variable). If that folder has no image, it
offers to download the Deluge community firmware release and use it.

Options:
  --sd <path>           Attach an SD card. <path> may be a raw FAT image file
                        (attached directly) or a directory, which is snapshotted
                        into a temporary FAT image at launch. If the directory
                        name ends in '_rw', the guest's changes are written back
                        to it on exit. SD folders need the bundled mkfs.fat and
                        mcopy tools. If omitted, an 'sdcard_rw' or 'sdcard'
                        folder in the current directory is used automatically;
                        if neither exists, the launcher offers to download the
                        Synthstrom factory card contents into .\sdcard.
  --midi <chardev>      Back SCIF0 (DIN MIDI) with a QEMU chardev spec, e.g.
                        --midi udp:127.0.0.1:1999. ('coremidi' is macOS-only.)
  --usb-midi <chardev>  Attach a host USB-MIDI device on a QEMU chardev spec.
  --audio <driver>      Select a host audio backend (default: dsound). e.g.
                        --audio sdl / wav / none. 'auto' selects dsound.
  --audio-buffer <ms>   Device staging-FIFO cushion in milliseconds (default 15).
                        Separate from the host playback buffer, which defaults to
                        80 ms on Windows (override with DELUGEMU_AUDIO_HOST_BUFFER_US)
                        to ride out main-loop/skin-upload stalls; raise either if
                        you hear dropouts.
  --tx-render-head <addr|auto|off>
                        Bound audio ring reads by the firmware's render head so
                        overload degrades to brief clean gaps instead of
                        distortion. Defaults to 'auto' (the emulator logs the
                        address it locks). Pass a guest address (hex/decimal)
                        to pin it, or 'off' for the raw play head.
  --skin-refresh-ms <ms>
                        Front-panel UI refresh interval (default 50, ~20fps).
                        Raise it to lower the host redraw rate and free the main
                        loop for audio under heavy live load. Range 10-1000.
  --icount [<shift>]    Run on a deterministic instruction-counted virtual clock
                        (clean-but-slower audio under load). Off by default.
  --display <mode>      console (front-panel window, default) | headless | none.
  --inverse             Use the dark skin (Delugemu_Inverse.png) with black
                        unlit pads; default is the light skin with white pads.
  -h, --help            Show this help and exit.

Anything after a literal `--`, or any unrecognised flag, is passed straight
through to qemu-system-arm.exe.
'@ | Write-Host
}

# --- Locate the emulator and assets ----------------------------------------

# Allow `--help` to work even before checking for the binary.
if ($CliArgs | Where-Object { $_ -eq '-h' -or $_ -eq '--help' }) { Show-Usage; exit 0 }

# Release tag for the window title / About box (VERSION is stamped into the
# bundle by package.sh).
if (-not $env:DELUGEMU_VERSION) {
    $verFile = Join-Path $Here 'VERSION'
    if (Test-Path -LiteralPath $verFile) {
        $env:DELUGEMU_VERSION = (Get-Content -LiteralPath $verFile -First 1).Trim()
    }
}

$Qemu = if ($env:DELUGEMU_QEMU_BIN) { $env:DELUGEMU_QEMU_BIN } else { Join-Path $Here 'qemu-system-arm.exe' }
if (-not (Test-Path -LiteralPath $Qemu)) {
    Die "qemu-system-arm.exe not found at $Qemu"
}

$SkinImage = if ($env:DELUGEMU_SKIN) { $env:DELUGEMU_SKIN } else { $null }
$Inverse   = $false
$FirmwareDir = if ($env:DELUGEMU_FIRMWARE_DIR) { $env:DELUGEMU_FIRMWARE_DIR }
               elseif ($AppMode) { Join-Path $StateDir 'firmware' }
               else { Join-Path $Here 'firmware' }

$CommunityFwUrl  = 'https://github.com/SynthstromAudible/DelugeFirmware/releases/download/release_1_2_1/deluge-community-release-1_2_1.zip'
$CommunityFwName = 'Deluge community firmware 1.2.1 (Chopin)'

# Default SD-card folder (auto-detected when no --sd is given) and the Synthstrom
# factory card contents offered for download when neither it nor an '<sd>_rw'
# variant exists. Downloaded and unzipped, not redistributed.
# In app mode the default card folder is '_rw' so songs the user saves in the
# emulator persist across runs (guest changes are written back on exit).
$SdDir         = if ($env:DELUGEMU_SD_DIR) { $env:DELUGEMU_SD_DIR }
                 elseif ($AppMode) { Join-Path $StateDir 'sdcard_rw' }
                 else { 'sdcard' }
$FactorySdUrl  = 'https://synthstrom-audible-deluge.s3.us-east-2.amazonaws.com/Deluge+OLED+V4p1p0+factory+card+contents.zip'
$FactorySdName = 'Deluge OLED V4.1.0 factory card contents'

# A bundled or on-PATH tool. Returns the resolved path or $null.
function Find-Tool {
    param([string]$Name)
    $local = Join-Path $Here $Name
    if (Test-Path -LiteralPath $local) { return $local }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

# --- Firmware resolution ----------------------------------------------------

# Return a usable firmware image in $FirmwareDir, or $null. Prefer an OLED .bin
# (the emulator's OLED also renders the 7-seg UI), then any .bin, then an .elf.
function Find-LocalFirmware {
    if (-not (Test-Path -LiteralPath $FirmwareDir)) { return $null }
    $bins = Get-ChildItem -LiteralPath $FirmwareDir -Recurse -File -Filter '*.bin' -ErrorAction SilentlyContinue
    $oled = $bins | Where-Object { $_.Name -notmatch '7seg' } | Sort-Object FullName | Select-Object -First 1
    if ($oled) { return $oled.FullName }
    $anyBin = $bins | Sort-Object FullName | Select-Object -First 1
    if ($anyBin) { return $anyBin.FullName }
    $elf = Get-ChildItem -LiteralPath $FirmwareDir -Recurse -File -Filter '*.elf' -ErrorAction SilentlyContinue |
        Sort-Object FullName | Select-Object -First 1
    if ($elf) { return $elf.FullName }
    return $null
}

function Get-CommunityFirmware {
    New-Item -ItemType Directory -Force -Path $FirmwareDir | Out-Null
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("delugemu-fw-" + [System.IO.Path]::GetRandomFileName())
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $zip = Join-Path $tmp 'firmware.zip'
    try {
        Notify "Downloading $CommunityFwName - the app will open when everything is ready"
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $CommunityFwUrl -OutFile $zip -UseBasicParsing
        Write-Log "Extracting into $FirmwareDir"
        Expand-Archive -LiteralPath $zip -DestinationPath $FirmwareDir -Force
    }
    catch {
        Die "firmware download/extract failed: $($_.Exception.Message)"
    }
    finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# --- SD card ----------------------------------------------------------------

$script:SdTmpImg    = $null
$script:SdFolder    = $null
$script:SdWriteback = $false

# Compute the power-of-two image size (MiB) for a content folder, mirroring
# mksd.sh: content + 30% slack + 64 MiB, floor 128 MiB, rounded up to 2^n.
function Get-SdImageSizeMiB {
    param([string]$Folder)
    $bytes = (Get-ChildItem -LiteralPath $Folder -Recurse -File -ErrorAction SilentlyContinue |
        Measure-Object -Property Length -Sum).Sum
    if (-not $bytes) { $bytes = 0 }
    $kib = [math]::Ceiling($bytes / 1024)
    $needMiB = [int]([math]::Floor(($kib * 13 / 10) / 1024) + 64)
    if ($needMiB -lt 128) { $needMiB = 128 }
    $imgMiB = 128
    while ($imgMiB -lt $needMiB) { $imgMiB *= 2 }
    return $imgMiB
}

# Build a raw FAT32 image from a folder using bundled mkfs.fat + mcopy.
function New-SdImageFromFolder {
    param([string]$Folder)
    $mkfs  = Find-Tool 'mkfs.fat.exe'
    $mcopy = Find-Tool 'mcopy.exe'
    if (-not $mkfs -or -not $mcopy) {
        Die ("SD folders need the bundled mkfs.fat and mcopy tools, which are " +
             "missing. Pass a raw .img with --sd instead, or use the MSYS2 build " +
             "(see docs\windows.md).")
    }
    $imgMiB = Get-SdImageSizeMiB -Folder $Folder
    $img = Join-Path ([System.IO.Path]::GetTempPath()) ("delugemu-sd-" + [System.IO.Path]::GetRandomFileName() + '.img')
    Notify "Preparing the SD card image ($imgMiB MiB - this can take a minute)..."

    # Pre-size the image, then format and populate it.
    $fs = [System.IO.File]::Open($img, 'Create', 'Write')
    try { $fs.SetLength([int64]$imgMiB * 1MB) } finally { $fs.Close() }

    & $mkfs '-F' '32' '-n' 'DELUGE' $img | Out-Null
    if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath $img -Force -EA SilentlyContinue; Die 'mkfs.fat failed' }

    $children = Get-ChildItem -LiteralPath $Folder -Force | ForEach-Object { $_.FullName }
    if ($children) {
        & $mcopy '-i' $img '-s' '-Q' '-o' @($children) '::/'
        if ($LASTEXITCODE -ne 0) { Remove-Item -LiteralPath $img -Force -EA SilentlyContinue; Die 'mcopy (populate) failed' }
    }

    $script:SdTmpImg = $img
    $script:SdFolder = $Folder.TrimEnd('\')
    if ($script:SdFolder -match '_rw$') {
        $script:SdWriteback = $true
        Write-Log "Folder name ends in '_rw': changes will be written back on exit"
    }
    # Tell the front panel where the folder-backed card lives so the Help
    # menu's "Open SD Card Folder" item can reveal it.
    $env:DELUGEMU_SD_FOLDER = (Resolve-Path -LiteralPath $script:SdFolder).Path
    return $img
}

# Mirror the (guest-modified) temporary image back into the source folder.
function Invoke-SdWriteback {
    if (-not $script:SdTmpImg) { return }
    if (-not (Test-Path -LiteralPath $script:SdFolder)) { return }
    $mcopy = Find-Tool 'mcopy.exe'
    if (-not $mcopy) { Write-Warn "mcopy not found; cannot write SD changes back"; return }
    Write-Log "Writing SD changes back to $($script:SdFolder)"
    $out = Join-Path ([System.IO.Path]::GetTempPath()) ("delugemu-sdout-" + [System.IO.Path]::GetRandomFileName())
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    try {
        & $mcopy '-i' $script:SdTmpImg '-s' '-n' '-m' '::/*' $out 2>$null
        # Robocopy mirrors the extracted tree back, deleting removed files.
        & robocopy $out $script:SdFolder /MIR /NJH /NJS /NDL /NP /NFL | Out-Null
    }
    catch { Write-Warn "write-back reported errors: $($_.Exception.Message)" }
    finally { Remove-Item -LiteralPath $out -Recurse -Force -ErrorAction SilentlyContinue }
}

function Resolve-SdArgs {
    param([string]$Path)
    if (Test-Path -LiteralPath $Path -PathType Container) {
        $img = New-SdImageFromFolder -Folder $Path
        return @('-drive', "if=sd,format=raw,file=$img")
    }
    elseif (Test-Path -LiteralPath $Path -PathType Leaf) {
        return @('-drive', "if=sd,format=raw,file=$Path")
    }
    else {
        Die "SD image/directory not found: $Path"
    }
}

# Download the Synthstrom factory card zip and unzip it into $Dir.
function Get-FactorySdCard {
    param([string]$Dir)
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("delugemu-sddl-" + [System.IO.Path]::GetRandomFileName())
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $zip = Join-Path $tmp 'sdcard.zip'
    try {
        Notify "Downloading $FactorySdName - the app will open when everything is ready"
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $FactorySdUrl -OutFile $zip -UseBasicParsing
        Write-Log "Extracting into $Dir"
        Expand-Archive -LiteralPath $zip -DestinationPath $Dir -Force
    }
    catch {
        Die "SD card download/extract failed: $($_.Exception.Message)"
    }
    finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# --- Argument parsing (mirrors run.sh) --------------------------------------

$Firmware      = $null
$Midi          = $null
$UsbMidi       = $null
$Audio         = $null
$AudioBuffer   = $null
# Render-head clamp defaults to 'auto'; the emulator logs the address the
# auto-detection locks (or that it gave up). Pass --tx-render-head off to
# track the raw play head instead.
$TxRenderHead  = 'auto'
$SkinRefreshMs = $null
$Icount        = $null
$DisplayMode   = 'console'
$SdArgs        = @()
$Extra         = @()

$argv = @($CliArgs)
$i = 0
# A leading non-flag token is the firmware path.
if ($argv.Count -gt 0 -and $argv[0] -notmatch '^-') {
    $Firmware = $argv[0]; $i = 1
}
while ($i -lt $argv.Count) {
    $a = $argv[$i]
    switch -regex ($a) {
        '^--$'            { $Extra += $argv[($i + 1)..($argv.Count - 1)]; $i = $argv.Count; break }
        '^(-h|--help)$'   { Show-Usage; exit 0 }
        '^--sd$'          { if ($i + 1 -ge $argv.Count) { Die '--sd requires a path' }; $SdArgs = Resolve-SdArgs $argv[$i + 1]; $i += 2; break }
        '^--sd=(.+)$'     { $SdArgs = Resolve-SdArgs $Matches[1]; $i += 1; break }
        '^--midi$'        { if ($i + 1 -ge $argv.Count) { Die '--midi requires a chardev spec' }; $Midi = $argv[$i + 1]; $i += 2; break }
        '^--midi=(.+)$'   { $Midi = $Matches[1]; $i += 1; break }
        '^--usb-midi$'    { if ($i + 1 -ge $argv.Count) { Die '--usb-midi requires a chardev spec' }; $UsbMidi = $argv[$i + 1]; $i += 2; break }
        '^--usb-midi=(.+)$' { $UsbMidi = $Matches[1]; $i += 1; break }
        '^--audio$'       { if ($i + 1 -ge $argv.Count) { Die '--audio requires a driver name' }; $Audio = $argv[$i + 1]; $i += 2; break }
        '^--audio=(.+)$'  { $Audio = $Matches[1]; $i += 1; break }
        '^--audio-buffer$' { if ($i + 1 -ge $argv.Count) { Die '--audio-buffer requires a value in ms' }; $AudioBuffer = $argv[$i + 1]; $i += 2; break }
        '^--audio-buffer=(.+)$' { $AudioBuffer = $Matches[1]; $i += 1; break }
        '^--tx-render-head$' { if ($i + 1 -ge $argv.Count) { Die '--tx-render-head requires a guest address, auto, or off' }; $TxRenderHead = $argv[$i + 1]; $i += 2; break }
        '^--tx-render-head=(.+)$' { $TxRenderHead = $Matches[1]; $i += 1; break }
        '^--skin-refresh-ms$' { if ($i + 1 -ge $argv.Count) { Die '--skin-refresh-ms requires a value in ms' }; $SkinRefreshMs = $argv[$i + 1]; $i += 2; break }
        '^--skin-refresh-ms=(.+)$' { $SkinRefreshMs = $Matches[1]; $i += 1; break }
        '^--icount$'      { if (($i + 1 -lt $argv.Count) -and ($argv[$i + 1] -notmatch '^-')) { $Icount = $argv[$i + 1]; $i += 2 } else { $Icount = 'auto'; $i += 1 }; break }
        '^--icount=(.+)$' { $Icount = $Matches[1]; $i += 1; break }
        '^--display$'     { if ($i + 1 -ge $argv.Count) { Die '--display requires a mode' }; $DisplayMode = $argv[$i + 1]; $i += 2; break }
        '^--display=(.+)$' { $DisplayMode = $Matches[1]; $i += 1; break }
        '^--inverse$'     { $Inverse = $true; $i += 1; break }
        default           { $Extra += $a; $i += 1; break }
    }
}

# --- Resolve firmware -------------------------------------------------------

if (-not $Firmware) {
    $Firmware = Find-LocalFirmware
    if ($Firmware) {
        Write-Log "No firmware given; using $Firmware"
    }
    elseif ([Environment]::UserInteractive) {
        if (Confirm-User "No firmware specified and none found in $FirmwareDir.`nDownload $CommunityFwName (~900 KB) from Synthstrom and use it?") {
            Get-CommunityFirmware
            $Firmware = Find-LocalFirmware
            if (-not $Firmware) { Die 'firmware download/extract produced no .bin image' }
            Write-Log "Using $Firmware"
        }
        else {
            Show-Usage
            Die "no firmware. Pass a path or place a .bin in $FirmwareDir."
        }
    }
    else {
        Show-Usage
        Die "no firmware given and none found in $FirmwareDir"
    }
}
if (-not (Test-Path -LiteralPath $Firmware -PathType Leaf)) { Die "Firmware not found: $Firmware" }

# Tell the front panel where the firmware folder lives so the Help menu's
# "Open Firmware Folder" item can reveal it.
if (Test-Path -LiteralPath $FirmwareDir -PathType Container) {
    $env:DELUGEMU_FW_FOLDER = (Resolve-Path -LiteralPath $FirmwareDir).Path
}

# --- Resolve default SD card ------------------------------------------------

# If no --sd was given, auto-detect an '<sd>_rw' or '<sd>' folder in the current
# directory (the '_rw' variant is preferred so guest changes are written back).
# If neither exists and we are interactive, offer to download the factory card.
if ($SdArgs.Count -eq 0) {
    foreach ($cand in @("${SdDir}_rw", $SdDir)) {
        if (Test-Path -LiteralPath $cand -PathType Container) {
            Write-Log "No --sd given; defaulting to .\$cand"
            $SdArgs = Resolve-SdArgs $cand
            break
        }
    }
}
if ($SdArgs.Count -eq 0 -and [Environment]::UserInteractive) {
    if (Confirm-User "No SD card specified and no $SdDir folder found.`nDownload $FactorySdName from Synthstrom and use it?`n(The download and first card build take a few minutes; the app window opens when they finish.)") {
        Get-FactorySdCard -Dir $SdDir
        if (Test-Path -LiteralPath $SdDir -PathType Container) {
            $SdArgs = Resolve-SdArgs $SdDir
        }
        else {
            Write-Warn "factory card download produced no .\$SdDir folder; continuing without an SD card"
        }
    }
    else {
        Write-Log "Continuing without an SD card"
    }
}

# --- Build QEMU arguments ---------------------------------------------------

# SCIF0 (DIN MIDI) + monitor.
$SerialArgs = @()
if ($Midi -eq 'coremidi') {
    Die "'coremidi' MIDI is macOS-only. Use a chardev spec, e.g. --midi udp:127.0.0.1:1999 (see docs\windows.md)."
}
elseif ($Midi) {
    $SerialArgs = @('-serial', $Midi, '-monitor', 'stdio')
    Write-Log "Routing SCIF0/MIDI to chardev: $Midi"
}
else {
    $SerialArgs = @('-serial', 'mon:stdio')
}

# Host USB-MIDI device on serial slot 1.
$UsbMidiArgs = @()
if ($UsbMidi -eq 'coremidi') {
    Die "'coremidi' USB-MIDI is macOS-only. Use a chardev spec, e.g. --usb-midi udp:127.0.0.1:1998."
}
elseif ($UsbMidi) {
    $UsbMidiArgs = @('-serial', $UsbMidi, '-global', 'rza1l-usb.midi=on')
    Write-Log "Attaching host USB-MIDI device on chardev: $UsbMidi"
}

# Display mode.
$DisplayArgs = @()
switch ($DisplayMode) {
    'headless' { $DisplayArgs = @('-nographic') }
    'console'  { $DisplayArgs = @() }   # QEMU opens its default GUI window
    'none'     { $DisplayArgs = @('-display', 'none') }
    default    { Die "unknown --display mode '$DisplayMode' (console|headless|none)" }
}

# Front-panel skin image (console only). Default to the light Normal skin;
# --inverse selects the original dark skin with black unlit pads. An explicit
# DELUGEMU_SKIN overrides the file choice (but --inverse still sets the theme).
if (-not $SkinImage) {
    $SkinImage = if ($Inverse) { Join-Path $Here 'Delugemu_Inverse.png' }
                 else          { Join-Path $Here 'Delugemu_Normal.png' }
}
$SkinArgs = @()
if ($DisplayMode -eq 'console') {
    if (Test-Path -LiteralPath $SkinImage) {
        $SkinArgs = @('-global', "deluge-skin.image=$SkinImage")
        if ($Inverse) { $SkinArgs += @('-global', 'deluge-skin.inverse=on') }
    }
    else {
        Write-Warn "skin image not found at $SkinImage; the panel will render without its photo background"
    }
    # Front-panel refresh interval. A longer interval lowers the host redraw rate
    # and frees QEMU's main loop for audio service under heavy load (see the audio
    # host-buffer note below); a shorter one makes the panel smoother.
    if ($SkinRefreshMs) {
        if ($SkinRefreshMs -notmatch '^\d+$') { Die '--skin-refresh-ms must be a positive integer (ms)' }
        $SkinArgs += @('-global', "deluge-skin.refresh-ms=$SkinRefreshMs")
    }
}

# Audio backend. On Windows QEMU's implicit default audiodev does not reliably
# bind the SSIF to a working output, so we always pass an explicit -audiodev and
# wire it to the SSIF. Defaults to dsound; override with --audio (e.g. sdl, wav,
# none).
#
# Host playback buffer (out.buffer-length, microseconds): the dsound voice is
# pumped from QEMU's main loop -- the same thread that recomposites the
# front-panel skin -- so a periodic full-frame skin upload can briefly stall the
# audio service. A generous host buffer lets the OS audio DMA ride those stalls
# without an audible gap. Default 80 ms; override with DELUGEMU_AUDIO_HOST_BUFFER_US.
# This is independent of the device's own staging cushion (--audio-buffer / prime-ms).
$AudioHostBufferUs = if ($env:DELUGEMU_AUDIO_HOST_BUFFER_US) { $env:DELUGEMU_AUDIO_HOST_BUFFER_US } else { '80000' }
if ($AudioHostBufferUs -notmatch '^\d+$') { Die 'DELUGEMU_AUDIO_HOST_BUFFER_US must be a non-negative integer (microseconds)' }

$AudioArgs = @()
if (-not $Audio -or $Audio -eq 'auto') { $Audio = 'dsound' }
if ($Audio -eq 'none') {
    $AudioArgs = @('-audiodev', 'none,id=deluge0', '-global', 'rza1l-ssif.audiodev=deluge0')
    Write-Log "Routing SSIF audio to backend: none"
}
else {
    $AudioArgs = @('-audiodev', "$Audio,id=deluge0,out.buffer-length=$AudioHostBufferUs",
                   '-global', 'rza1l-ssif.audiodev=deluge0')
    Write-Log "Routing SSIF audio to backend: $Audio (host buffer $AudioHostBufferUs us)"
}
if ($AudioBuffer) {
    if ($AudioBuffer -notmatch '^\d+$') { Die '--audio-buffer must be a non-negative integer (ms)' }
    $AudioArgs += @('-global', "rza1l-ssif.prime-ms=$AudioBuffer")
    Write-Log "SSIF output buffer cushion: $AudioBuffer ms"
}

# Render-head clamp (default 'auto'; --tx-render-head off to disable). Bounds ring
# reads by the firmware's render head so overload becomes brief clean gaps rather
# than distortion. 'auto' is best-effort and falls back to the play head.
if ($TxRenderHead -and $TxRenderHead -ne 'off' -and $TxRenderHead -ne 'none') {
    if ($TxRenderHead -eq 'auto') {
        $AudioArgs += @('-global', 'rza1l-ssif.tx-render-head-auto=on')
        Write-Log "SSIF audio bounded by auto-detected firmware render head"
    }
    elseif ($TxRenderHead -match '^(0x[0-9A-Fa-f]+|\d+)$') {
        $AudioArgs += @('-global', "rza1l-ssif.tx-render-head=$TxRenderHead")
        Write-Log "SSIF audio bounded by firmware render head at: $TxRenderHead"
    }
    else {
        Die "--tx-render-head must be a guest address (hex 0x..., decimal), 'auto', or 'off'"
    }
}

# Optional deterministic instruction-counted clock (-icount). Paces the guest to a
# virtual clock: audio stays internally consistent (no dropouts) but the guest is
# capped to <= real time, so heavy DSP load slows playback instead of breaking it.
$IcountArgs = @()
if ($Icount) {
    if ($Icount -notmatch '^(auto|\d{1,2})$') { Die "--icount shift must be 'auto' or an integer (e.g. 0-20)" }
    $IcountArgs = @('-icount', "shift=$Icount,sleep=on")
    Write-Log "Deterministic icount clock enabled: shift=$Icount (guest capped to real time)"
}

# --- Launch -----------------------------------------------------------------

$qemuArgs = @('-M', 'deluge', '-kernel', $Firmware) +
    $SerialArgs + $UsbMidiArgs + $DisplayArgs + $SkinArgs + $AudioArgs + $IcountArgs + $SdArgs + $Extra

Hide-Splash
Write-Log "Launching deluge machine with $Firmware (display=$DisplayMode)"
try {
    & $Qemu @qemuArgs
    $code = $LASTEXITCODE
}
finally {
    if ($script:Tray) { $script:Tray.Visible = $false; $script:Tray.Dispose() }
    if ($script:SdTmpImg) {
        if ($script:SdWriteback) { Invoke-SdWriteback }
        Remove-Item -LiteralPath $script:SdTmpImg -Force -ErrorAction SilentlyContinue
    }
}
exit $code
