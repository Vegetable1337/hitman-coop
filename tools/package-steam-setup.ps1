# Builds the standalone installer for the Steam invitations preview.
param(
    [Parameter(Mandatory = $true)][string]$ModDll,
    [string]$SdkDir = (Join-Path $PSScriptRoot "..\_deps\sdk-release"),
    [string]$OutDir = (Join-Path $PSScriptRoot "..\_dist"),
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+-steam\.[0-9]+$')]
    [string]$Release = "0.14.0-steam.2"
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ModDll = (Resolve-Path -LiteralPath $ModDll).Path
$SdkDir = (Resolve-Path -LiteralPath $SdkDir).Path
$Pdb = [IO.Path]::ChangeExtension($ModDll, ".pdb")
$SdkDll = Join-Path $SdkDir "ZHMModSDK.dll"
if ((Get-Item -LiteralPath $SdkDll).VersionInfo.FileVersion -notmatch '^4\.2\.1(?:\.0)?$') {
    throw "Use ZHMModSDK 4.2.1 from tools/fetch-sdk.ps1."
}
& (Join-Path $PSScriptRoot "check-dll.ps1") -ModDll $ModDll -SdkDll $SdkDll

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
$Temp = (New-Item -ItemType Directory -Path (Join-Path $OutDir ("steam-setup-" + [guid]::NewGuid().ToString("N")))).FullName
try {
    $Stage = Join-Path $Temp "payload"
    $Files = Join-Path $Stage "files"
    $Mods = Join-Path $Files "mods"
    $Licenses = Join-Path $Stage "licenses"
    New-Item -ItemType Directory -Path $Mods, $Licenses | Out-Null

    foreach ($Item in Get-ChildItem -LiteralPath $SdkDir) {
        if ($Item.Name -in @("mods", "licenses", "LICENSE") -or $Item.Extension -eq ".pdb") { continue }
        Copy-Item -LiteralPath $Item.FullName -Destination $Files -Recurse
    }
    Copy-Item -LiteralPath $ModDll -Destination (Join-Path $Mods "HitmanCoop.dll")
    & python -I (Join-Path $PSScriptRoot "symbolize.py") $ModDll $Pdb --export (Join-Path $Mods "HitmanCoop.sym")
    if ($LASTEXITCODE -ne 0) { throw "Failed to generate HitmanCoop.sym." }
    Copy-Item -LiteralPath (Join-Path $Root "LICENSE") -Destination (Join-Path $Licenses "HitmanCoop-LICENSE.txt")
    Copy-Item -LiteralPath (Join-Path $SdkDir "LICENSE") -Destination (Join-Path $Licenses "ZHMModSDK-LICENSE.txt")
    if (Test-Path -LiteralPath (Join-Path $SdkDir "licenses")) {
        Copy-Item -LiteralPath (Join-Path $SdkDir "licenses") -Destination (Join-Path $Licenses "ZHMModSDK-third-party") -Recurse
    }
    @(
        "HITMAN Co-op (GPL-3.0-or-later): https://github.com/Vegetable1337/hitman-coop/tree/v$Release"
        "ZHMModSDK 4.2.1 (GPL-3.0): https://github.com/OrfeasZ/ZHMModSDK/tree/v4.2.1"
    ) | Set-Content -LiteralPath (Join-Path $Licenses "SOURCES.txt") -Encoding ascii

    $Sums = foreach ($Item in Get-ChildItem -LiteralPath $Files -Recurse -File) {
        $Hash = (Get-FileHash -LiteralPath $Item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        "$Hash $($Item.FullName.Substring($Stage.Length + 1).Replace('\', '/'))"
    }
    Set-Content -LiteralPath (Join-Path $Stage "checksums.txt") -Value $Sums -Encoding ascii
    $Payload = Join-Path $Temp "payload.zip"
    Compress-Archive -LiteralPath (Get-ChildItem -LiteralPath $Stage).FullName -DestinationPath $Payload

    $SetupSrc = Join-Path $Root "installer\setup"
    $Csc = Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319\csc.exe"
    $FxDir = Split-Path $Csc
    $Name = "HitmanCoop-Setup-$Release.exe"
    $Built = Join-Path $Temp $Name
    $Sources = (Get-ChildItem -LiteralPath $SetupSrc -Filter *.cs).FullName
    & $Csc /nologo /codepage:65001 /target:winexe /optimize+ /platform:anycpu /define:STEAM_PREVIEW "/out:$Built" `
        "/win32icon:$SetupSrc\setup.ico" "/win32manifest:$SetupSrc\setup.manifest" "/resource:$Payload,payload.zip" `
        /r:System.Windows.Forms.dll /r:System.Drawing.dll "/r:$FxDir\System.IO.Compression.dll" `
        "/r:$FxDir\System.IO.Compression.FileSystem.dll" /r:System.Web.Extensions.dll @Sources
    if ($LASTEXITCODE -ne 0) { throw "Failed to build the Steam setup." }

    $Output = Join-Path $OutDir $Name
    Copy-Item -LiteralPath $Built -Destination $Output -Force
    $Hash = (Get-FileHash -LiteralPath $Output -Algorithm SHA256).Hash.ToLowerInvariant()
    "$Hash  $Name" | Set-Content -LiteralPath "$Output.sha256" -Encoding ascii
    Write-Output $Output
} finally {
    # The staging folder has a unique name and is a direct child of the output directory.
    if ((Split-Path -Parent $Temp).TrimEnd('\') -eq $OutDir.TrimEnd('\')) {
        Remove-Item -LiteralPath $Temp -Recurse -Force
    }
}
