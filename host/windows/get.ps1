# Frametop host for Windows, in one line. In PowerShell on the PC (no admin needed: the setup
# asks Windows for it):
#
#   irm https://raw.githubusercontent.com/Frametop/frametop/main/host/windows/get.ps1 | iex
#
# It downloads the host kit (frametop-host-setup.ps1 and "Setup Frametop host.cmd") from the
# same branch into %LOCALAPPDATA%\Frametop\host-kit and runs its setup, which works as
# host/windows/README.md says: it opens an admin window, installs Vibepollo if it isn't there,
# puts in Frametop's build after checking its SHA-256, and asks for the Web UI login. Run it
# again for a newer kit. With the setup's options (-Check, -Undo, -SkipLogin):
#
#   & ([scriptblock]::Create((irm https://raw.githubusercontent.com/Frametop/frametop/main/host/windows/get.ps1))) -Check
#
# or run "Setup Frametop host.cmd" in that folder with them. raw.githubusercontent.com, not
# frametop.github.io: GitHub Pages serves .ps1 files as application/octet-stream, which
# PowerShell 7's irm may hand back as bytes, not a script.
& {
    $ErrorActionPreference = "Stop"
    $ProgressPreference = "SilentlyContinue"
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    # FRAMETOP_HOST_BASE: another branch's kit, such as a fix to test before it's released.
    $base = if ($env:FRAMETOP_HOST_BASE) { $env:FRAMETOP_HOST_BASE.TrimEnd("/") } else { "https://raw.githubusercontent.com/Frametop/frametop/main/host/windows" }
    # Per user, so no other account can change the script before it runs as admin.
    $kit = Join-Path $env:LOCALAPPDATA "Frametop\host-kit"
    New-Item -ItemType Directory -Force $kit | Out-Null
    foreach ($name in @("frametop-host-setup.ps1", "Setup Frametop host.cmd")) {
        $file = Join-Path $kit $name
        Invoke-WebRequest -UseBasicParsing -Uri ("$base/" + [Uri]::EscapeDataString($name)) -OutFile "$file.part"
        Move-Item -Force "$file.part" $file
    }
    Write-Host "Frametop host kit: $kit"
    # Its own process: the setup ends with exit, which would close this window.
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $kit "frametop-host-setup.ps1") @args
} @args
