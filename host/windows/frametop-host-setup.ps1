# Frametop host setup for Windows: makes this PC a host for Frametop's remote displays, its
# monitors shown as screens on a Steam Frame (docs/remote-displays.md). Run
# "Setup Frametop host.cmd"; it asks Windows for admin.
#
#  1. Vibepollo 2.0.3 (github.com/Nonary/Vibepollo), installed with its own installer if it
#     isn't here, or if this PC has an older Vibepollo or an older Frametop build on one
#     (checked against its SHA-256 first).
#  2. Frametop's build of Vibepollo (github.com/Frametop/frametop-vibepollo): its sunshine.exe,
#     and in the tools folder the capture helper that draws an app window and its popups and
#     the clipboard reader. It streams any of your monitors and single app windows, lends the
#     PC's keyboard and mouse, and shares the clipboard (host/windows/README.md). Downloaded
#     from its release and each file checked against its SHA-256; the originals are kept next
#     to them as <name>.2.0.3-original.
#  3. Settings Frametop needs (the rest of Vibepollo's settings stay as they are): remote
#     displays carry the PC's sound, and a virtual display goes away when Frametop
#     disconnects it, but stays through a dropped stream.
#  4. The Web UI login you sign in with once from the Steam Frame: keep the one there is, or
#     set one. The password is typed here and never shown or saved.
#  5. Checks: Vibepollo's firewall rule covers every network type (a Steam Link dongle's
#     network counts as Public), whether a Steam Link dongle is plugged in, and that the Web
#     UI answers.
#
# Options:
#   -FrametopBuild DIR|URL    where Frametop's build comes from: a folder or release URL with
#                             sunshine.exe, sunshine_wgc_capture.exe and frametop_clipboard.exe
#                             (default: BuildUrl below)
#   -SkipLogin                leave the Web UI login alone
#   -Check                    only say what would change
#   -Undo                     put back the original sunshine.exe and the settings from before
# A log goes to %ProgramData%\Frametop\host-setup.log, backups to %ProgramData%\Frametop\backup-*.
param([string]$FrametopBuild = "", [switch]$SkipLogin, [switch]$Check, [switch]$Undo)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$VibepolloVersion = "2.0.3"
$SetupUrl = "https://github.com/Nonary/Vibepollo/releases/download/2.0.3/VibepolloSetup-v2.0.3.exe"
$SetupSha = "D099E2A686D6E4A61A1F8D48E04C2A2FDA2FE27BCFD7D50EF2365758B09AAB05"
# Frametop's build: Frametop/frametop-vibepollo release frametop-2.0.4 (its CI, from the tag).
# Each file: where it goes in the install, its SHA-256, and the SHA-256 of Vibepollo 2.0.3's own
# (none: 2.0.3 has no such file).
$BuildUrl = "https://github.com/Frametop/frametop-vibepollo/releases/download/frametop-2.0.4"
$BuildFiles = @(
    @{ Path = "sunshine.exe"; Sha = "F3CFDCC2FB22519F024CBC1969E9BC77A9BB7392FF47E6B3212C844BCE681BDF"; Stock = "37100D36E920B84899898FEB57EF234702329CE9A7A02A9321AE47F7CF3A132A" }
    @{ Path = "tools\sunshine_wgc_capture.exe"; Sha = "E16C4EAE337E87297D897B7CBC672B4BCF93BD634AC852710AC655AFAEFF26BB"; Stock = "E6A4EE79E0B8CC71E2227277B99AF25FFFFC57DFF0DFFFEDE042B8C56EFB6066" }
    @{ Path = "tools\frametop_clipboard.exe"; Sha = "2E020870D9933F017F6DFEBF65ABEAEFFD41B04765C44DC3A7454F9D3780ABDD"; Stock = $null }
)
# Earlier Frametop builds on Vibepollo 2.0.3 (their sunshine.exe), replaced by this one.
$OlderBuildShas = @()
# sunshine.exe of earlier setups, which Vibepollo 2.0.3's installer replaces first: Vibepollo
# 2.0.0's own and the Frametop builds the 2.0.0 kits put in, and Frametop's 2.0.3 build.
$EarlierShas = @(
    "2CC018FD92DDB4D3748D91D8DA25316909ED45DB3710D1FF278BD73D51EB00C1"  # Vibepollo 2.0.0
    "B5B7D2E7353454AEA6D895D0B68DE235E4CC581684C9DD44F8EFAF2E872F517F"  # 2f032252, built by hand without WebRTC
    "6AF4F34503C7F6F84D1F6967D9FCEFAF2BD8145B34044C992B5E26A614E37A91"  # a84b6cfc, CI, before the HDR-off guard
    "57EECCF9CA6ECEC4F0C5AEBBB27AAFE711A700C9E0986601344A52682856E94E"  # frametop-2.0.0-1, before the resolution switch
    "2BF7D1DDBE4350947D58A459A47FF5AE85ED0EFCA957ED7D91F9B582FC37599D"  # frametop-2.0.0-2
    "0023E61B90C4F19D095AC631BF6A1E9B128DD2B477E953BF78D58E2B7BF778B7"  # frametop-2.0.0-3
    "36FC18277B1325936304FE1E3D736D73ACF42EEE219F64B10038873D2F10C2E3"  # frametop-2.0.3
)
$Settings = [ordered]@{
    "remote_monitor_mute_audio"                      = "disabled"
    "remote_monitor_disconnect_on_client_disconnect" = "enabled"
    "remote_monitor_disconnect_on_stream_end"        = "disabled"
}
$Service = "ApolloService"  # Vibepollo keeps Apollo's service name
$Data = Join-Path $env:ProgramData "Frametop"
$Here = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }

$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $pass = @()
    foreach ($k in $PSBoundParameters.Keys) {
        $v = $PSBoundParameters[$k]
        if ($v -is [switch]) { if ($v) { $pass += "-$k" } } else { $pass += "-$k `"$v`"" }
    }
    Start-Process powershell -Verb RunAs -ArgumentList ("-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" " + ($pass -join " "))
    exit
}
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
New-Item -ItemType Directory -Force $Data | Out-Null
$Log = Join-Path $Data "host-setup.log"

function Say([string]$text) {
    Write-Host $text
    Add-Content -Path $Log -Value ((Get-Date -Format "yyyy-MM-dd HH:mm:ss") + "  " + $text)
}

function Sha([string]$path) { (Get-FileHash $path -Algorithm SHA256).Hash }

function Plain($secure) {
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) } finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
}

# One argument for a Windows program, quoted so it reads back exactly this string
# (Windows PowerShell 5 mangles arguments that contain double quotes).
function Quote([string]$s) {
    $r = '"'
    $slashes = 0
    foreach ($c in $s.ToCharArray()) {
        if ($c -eq '\') { $slashes++; continue }
        if ($c -eq '"') { $r += ('\' * (2 * $slashes + 1)) + '"' } else { $r += ('\' * $slashes) + $c }
        $slashes = 0
    }
    $r + ('\' * (2 * $slashes)) + '"'
}

function Find-Vibepollo {
    $keys = "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*", "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*"
    $entry = Get-ItemProperty $keys -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -eq "Vibepollo" -and $_.InstallLocation } | Select-Object -First 1
    $dir = if ($entry) { $entry.InstallLocation.TrimEnd('\') } else { "C:\Program Files\Apollo" }
    if ((Test-Path "$dir\sunshine.exe") -and (Get-Service $Service -ErrorAction SilentlyContinue)) { return $dir }
    return $null
}

function Stop-Vibepollo {
    Stop-Service $Service
    for ($i = 0; $i -lt 30 -and (Get-Process sunshine, sunshine_wgc_capture -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 500 }
}

# sunshine.conf: "key = value" lines. Returns the lines with $Settings applied.
function Set-Settings([string[]]$lines) {
    $out = New-Object System.Collections.Generic.List[string]
    $done = @{}
    foreach ($line in $lines) {
        $key = ($line -split "=", 2)[0].Trim()
        if ($Settings.Contains($key)) {
            if (-not $done[$key]) { $out.Add("$key = $($Settings[$key])"); $done[$key] = $true }
        } else {
            $out.Add($line)
        }
    }
    foreach ($key in $Settings.Keys) { if (-not $done[$key]) { $out.Add("$key = $($Settings[$key])") } }
    return , $out.ToArray()
}

# Where a file of the install keeps Vibepollo's own once Frametop's replaces it.
function Kept([string]$path) { "$path.$VibepolloVersion-original" }

# Whether the install is Vibepollo 2.0.3's: its own sunshine.exe, or a Frametop build with
# 2.0.3's kept (not a later Vibepollo installed over one, which leaves the kept file behind).
function On-Base([string]$dir) {
    $stock = $BuildFiles[0].Stock
    $now = Sha "$dir\sunshine.exe"
    if ($now -eq $stock) { return $true }
    $kept = Kept "$dir\sunshine.exe"
    return ((@($BuildFiles[0].Sha) + $OlderBuildShas) -contains $now) -and (Test-Path $kept) -and ((Sha $kept) -eq $stock)
}

# Frametop's build, every file checked: install path -> the file here. $null when it isn't
# available (a folder without it).
function Get-Build {
    $from = if ($FrametopBuild) { $FrametopBuild } else { $BuildUrl }
    $web = $from -match "^https?://"
    if ($web) {
        $folder = Join-Path $env:TEMP "frametop-build"
        New-Item -ItemType Directory -Force $folder | Out-Null
        Say "Downloading Frametop's build of Vibepollo..."
    }
    $files = @{}
    foreach ($f in $BuildFiles) {
        $name = Split-Path $f.Path -Leaf
        if ($web) {
            $file = Join-Path $folder $name
            Invoke-WebRequest -UseBasicParsing -Uri ($from.TrimEnd("/") + "/" + $name) -OutFile $file
        } else {
            $file = Join-Path $from $name
            if (-not (Test-Path $file)) { $file = Join-Path $from $f.Path }  # a build folder: tools\...
            if (-not (Test-Path $file)) { return $null }
        }
        if ((Sha $file) -ne $f.Sha) { throw "$file isn't Frametop's build of Vibepollo (its SHA-256 doesn't match)." }
        $files[$f.Path] = $file
    }
    return $files
}

function Wait-WebUi {
    Add-Type @"
using System.Net;
using System.Security.Cryptography.X509Certificates;
public class FrametopTrustLocal : ICertificatePolicy {
    public bool CheckValidationResult(ServicePoint s, X509Certificate c, WebRequest r, int p) { return true; }
}
"@ -ErrorAction SilentlyContinue
    $old = [Net.ServicePointManager]::CertificatePolicy
    [Net.ServicePointManager]::CertificatePolicy = New-Object FrametopTrustLocal  # 127.0.0.1 only
    try {
        for ($i = 0; $i -lt 40; $i++) {
            try { Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 "https://127.0.0.1:47990/" | Out-Null; return $true } catch { Start-Sleep 1 }
        }
        return $false
    } finally {
        [Net.ServicePointManager]::CertificatePolicy = $old
    }
}

function Main {
    Say "== Frametop host setup$(if ($Check) { ' (check only)' })$(if ($Undo) { ' (undo)' })"
    $dir = Find-Vibepollo

    if ($Undo) {
        if (-not $dir) { throw "Vibepollo isn't installed here." }
        $backup = Get-ChildItem $Data -Directory -Filter "backup-*" -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -First 1
        Stop-Vibepollo
        $exe = "$dir\sunshine.exe"
        $old = "$exe.2.0.0-original"  # a 2.0.0 kit's, on an install no 2.0.3 installer replaced
        if (-not (Test-Path (Kept $exe)) -and (Test-Path $old) -and ($EarlierShas -contains (Sha $exe))) {
            Copy-Item $old $exe -Force
            Say "Put back the original sunshine.exe (Vibepollo 2.0.0)."
        }
        foreach ($f in $BuildFiles) {
            $path = Join-Path $dir $f.Path
            if (Test-Path (Kept $path)) {
                Copy-Item (Kept $path) $path -Force
                Say "Put back the original $($f.Path)."
            } elseif (-not $f.Stock -and (Test-Path $path)) {
                Remove-Item $path -Force
                Say "Removed $($f.Path) (Vibepollo has none)."
            }
        }
        if ($backup -and (Test-Path "$($backup.FullName)\sunshine.conf")) {
            Copy-Item "$($backup.FullName)\sunshine.conf" "$dir\config\sunshine.conf" -Force
            Say "Put back the settings from $($backup.FullName)."
        }
        Start-Service $Service
        Say "Done. (The Web UI login stays as it is.)"
        return
    }

    # 1. Vibepollo 2.0.3: installed, or put in over an earlier setup Frametop's build replaced.
    $update = $false
    if ($dir -and -not (On-Base $dir)) {
        if ($EarlierShas -notcontains (Sha "$dir\sunshine.exe")) {
            $v = (Get-Item "$dir\sunshine.exe").VersionInfo.ProductVersion
            throw "This PC's sunshine.exe is Vibepollo $v, but neither Vibepollo $VibepolloVersion's own nor a Frametop build this setup knows (a build of your own, or a newer kit's), so it's left as it is. To start over, install Vibepollo $VibepolloVersion ($SetupUrl), then run this again."
        }
        $update = $true
    }
    if (-not $dir -or $update) {
        if ($Check) { Say "Would $(if ($update) { 'update Vibepollo to' } else { 'install Vibepollo' }) $VibepolloVersion."; return }
        $setup = Join-Path $env:TEMP "VibepolloSetup-v$VibepolloVersion.exe"
        Say "Downloading Vibepollo $VibepolloVersion..."
        Invoke-WebRequest -UseBasicParsing -Uri $SetupUrl -OutFile $setup
        if ((Sha $setup) -ne $SetupSha) { throw "The Vibepollo installer didn't match its SHA-256; not running it." }
        if ($update) { Say "Frametop's build now goes into Vibepollo $VibepolloVersion, and this PC has an earlier one." }
        Say "Running Vibepollo's installer: follow its steps, then come back here."
        Start-Process $setup -Wait
        $dir = Find-Vibepollo
        if (-not $dir) { throw "Vibepollo doesn't seem to be installed. Run this again once it is." }
        if (-not (On-Base $dir)) { throw "Vibepollo $VibepolloVersion's installer didn't put its sunshine.exe in place. Run this again once it has." }
    }
    $exe = "$dir\sunshine.exe"
    Say "Vibepollo: $dir"

    # 2. Frametop's build: the files that aren't it yet.
    $swap = @()
    foreach ($f in $BuildFiles) {
        $path = Join-Path $dir $f.Path
        $now = if (Test-Path $path) { Sha $path } else { $null }
        if ($now -eq $f.Sha) { continue }
        if ($f.Stock -and $now -ne $f.Stock -and -not (Test-Path (Kept $path))) {
            throw "This PC's $($f.Path) is neither Vibepollo $VibepolloVersion's nor Frametop's, so it's left as it is. To start over, reinstall Vibepollo $VibepolloVersion ($SetupUrl), then run this again."
        }
        $swap += $f
    }
    $build = $null
    if (-not $swap) {
        Say "Frametop's build of Vibepollo is already installed."
    } elseif (-not $Check) {
        $build = Get-Build
        if (-not $build) {
            Say "Frametop's build of Vibepollo isn't available here, so this PC can't stream its own monitors or app windows yet (virtual displays work)."
            $swap = @()
        }
    }

    # 3. Settings
    $conf = "$dir\config\sunshine.conf"
    $lines = if (Test-Path $conf) { [IO.File]::ReadAllLines($conf) } else { @() }
    $new = Set-Settings $lines
    $changed = ($new -join "`n") -ne ($lines -join "`n")

    # 4. Login
    $state = "$dir\config\sunshine_state.json"
    $user = $null
    if (Test-Path $state) { try { $user = (Get-Content $state -Raw | ConvertFrom-Json).username } catch { } }
    $password = $null
    if (-not $SkipLogin -and -not $Check) {
        if ($user) {
            $answer = Read-Host "The Web UI login is '$user'. Keep it? You'll sign in with it once from the Steam Frame. (Y/n)"
            $setLogin = $answer -match "^[nN]"
        } else {
            Write-Host "Vibepollo has no Web UI login yet. Make one: you'll sign in with it once from the Steam Frame."
            $setLogin = $true
        }
        if ($setLogin) {
            $typed = Read-Host "User name$(if ($user) { " (Enter keeps '$user')" })"
            if ($typed) { $user = $typed }
            while (-not $user) { $user = Read-Host "User name" }
            while ($true) {
                $a = Plain (Read-Host "Password" -AsSecureString)
                $b = Plain (Read-Host "Same password again" -AsSecureString)
                if ($a.Length -ge 4 -and $a -ceq $b) { $password = $a; break }
                Write-Host "They don't match, or it's shorter than 4 characters. Try again."
            }
            Remove-Variable a, b
        }
    }

    if ($Check) {
        Say "Would $(if ($swap) { "install Frametop's $(($swap | ForEach-Object { $_.Path }) -join ', ')" } else { 'not change Vibepollo' }), $(if ($changed) { 'change' } else { 'not change' }) the settings."
    } elseif ($swap -or $changed -or $password) {
        $backup = Join-Path $Data ("backup-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
        New-Item -ItemType Directory $backup | Out-Null
        foreach ($f in "$dir\config\sunshine.conf", "$dir\config\apps.json") { if (Test-Path $f) { Copy-Item $f $backup } }
        Say "Backed up the settings to $backup."
        Say "Stopping Vibepollo..."
        Stop-Vibepollo
        try {
            foreach ($f in $swap) {
                $path = Join-Path $dir $f.Path
                if ($f.Stock -and -not (Test-Path (Kept $path)) -and (Sha $path) -eq $f.Stock) { Copy-Item $path (Kept $path) }
                Copy-Item $build[$f.Path] $path -Force
            }
            if ($swap) {
                Say "Installed Frametop's build of Vibepollo: $(($swap | ForEach-Object { $_.Path }) -join ', ') (Vibepollo's own are kept next to them as <name>.$VibepolloVersion-original)."
            }
            if ($changed) {
                [IO.File]::WriteAllLines($conf, [string[]]$new)
                Say "Set: $(($Settings.Keys | ForEach-Object { "$_ = $($Settings[$_])" }) -join ', ')."
            }
            if ($password) {
                $info = New-Object Diagnostics.ProcessStartInfo
                $info.FileName = $exe
                $info.WorkingDirectory = $dir
                $info.Arguments = "--creds " + (Quote $user) + " " + (Quote $password)
                $info.UseShellExecute = $false
                $info.RedirectStandardOutput = $true
                $info.RedirectStandardError = $true
                $proc = [Diagnostics.Process]::Start($info)
                $info.Arguments = ""
                $proc.StandardOutput.ReadToEnd() | Out-Null
                $proc.StandardError.ReadToEnd() | Out-Null
                $proc.WaitForExit()
                Remove-Variable password
                if ($proc.ExitCode -ne 0) { throw "Setting the Web UI login failed (sunshine.exe --creds: exit code $($proc.ExitCode))." }
                Say "Web UI login set: '$user'."
            }
        } finally {
            Start-Service $Service
            Say "Vibepollo is running again."
        }
    }

    # 5. Checks
    $rules = @(Get-NetFirewallApplicationFilter -ErrorAction SilentlyContinue |
        Where-Object { [Environment]::ExpandEnvironmentVariables($_.Program) -ieq $exe } |
        Get-NetFirewallRule | Where-Object { $_.Enabled -eq "True" -and $_.Direction -eq "Inbound" -and $_.Action -eq "Allow" })
    $everywhere = $rules | Where-Object { $_.Profile -eq "Any" -or ($_.Profile -match "Private" -and $_.Profile -match "Public") }
    if ($everywhere) {
        Say "Firewall: Vibepollo is allowed on every network type."
    } elseif ($Check) {
        Say "Would add a firewall rule letting Vibepollo in on every network type."
    } else {
        New-NetFirewallRule -DisplayName "Vibepollo (Frametop)" -Program $exe -Direction Inbound -Action Allow -Profile Any | Out-Null
        Say "Firewall: added a rule letting Vibepollo in on every network type (a Steam Link dongle's network is Public)."
    }
    $dongle = Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.InterfaceDescription -match "For Valve" } | Select-Object -First 1
    if ($dongle -and $dongle.Status -eq "Up") {
        $ip = (Get-NetIPAddress -InterfaceIndex $dongle.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue | Select-Object -First 1).IPAddress
        Say "Steam Link dongle: connected$(if ($ip) { " ($ip)" }). Frametop can stream over it, past your router."
    } elseif ($dongle) {
        Say "Steam Link dongle: plugged in, not connected to the Steam Frame. Frametop uses your network until it is."
    } else {
        Say "Steam Link dongle: none. Frametop streams over your network."
    }
    if (-not $Check) {
        if (Wait-WebUi) { Say "The Web UI answers (https://localhost:47990)." } else { Say "The Web UI didn't answer yet; check that Vibepollo is running." }
    }

    $addresses = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object {
        $_.IPAddress -notmatch "^(127\.|169\.254\.)" -and $_.InterfaceAlias -notmatch "Tailscale|vEthernet|Loopback" -and
        (-not $dongle -or $_.InterfaceIndex -ne $dongle.ifIndex) } | ForEach-Object { $_.IPAddress }
    Write-Host ""
    Say "Done. On the Steam Frame: open Remote displays in Frametop Settings, Add computer, pick $([Net.Dns]::GetHostName()) ($($addresses -join ', ')), and sign in as '$user'."
}

try {
    Main
} catch {
    Say "Failed: $_"
    if (-not $Check -and (Get-Service $Service -ErrorAction SilentlyContinue) -and (Get-Service $Service).Status -ne "Running") {
        Start-Service $Service -ErrorAction SilentlyContinue
    }
}
if (-not $env:FRAMETOP_NO_PAUSE) { Read-Host "Press Enter to close" | Out-Null }
