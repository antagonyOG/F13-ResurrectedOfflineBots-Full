$ErrorActionPreference = 'Stop'
$supported = '5541268C88B6C02BFB8BDA2D4B07E3E04BB6A03CEF1C5E89163B1E9FBC32A430'
$root = $PSScriptRoot
$bin = Join-Path $root 'SummerCamp\Binaries\Win64'
$exe = Join-Path $bin 'SummerCamp-Win64-Shipping.exe'
$pak = Join-Path $root 'SummerCamp\Content\Paks'
$report = Join-Path $root 'OfflineBots-Diagnostic.txt'
$lines = [System.Collections.Generic.List[string]]::new()
function AddLine([string]$value) { $script:lines.Add($value); Write-Output $value }
function HashOrMissing([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return 'MISSING' }
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash
}
AddLine "Game root: $root"
$hash = HashOrMissing $exe
AddLine "SummerCamp-Win64-Shipping.exe SHA-256: $hash"
AddLine "GAME BUILD: $(if ($hash -eq $supported) {'SUPPORTED'} else {'UNSUPPORTED OR MISSING'})"
AddLine "Supported SHA-256: $supported"
foreach ($name in @('X3DAudio1_7.dll','ResurrectedOfflineBots.dll')) {
    $installed = Join-Path $bin $name
    $payload = Join-Path (Join-Path $PSScriptRoot 'ResurrectedOfflineBots') $name
    $installedHash = HashOrMissing $installed
    $payloadHash = HashOrMissing $payload
    AddLine "$name installed SHA-256: $installedHash"
    AddLine "$name package SHA-256: $payloadHash"
    AddLine "$name status: $(if ($installedHash -ne 'MISSING' -and $installedHash -eq $payloadHash) {'OK'} else {'MISSING OR HASH MISMATCH'})"
}
AddLine 'Selected bootstrap method: application-local X3DAudio1_7.dll proxy (version.dll not required)'
$versionPath = Join-Path $bin 'version.dll'
$versionHash = HashOrMissing $versionPath
AddLine "Existing version.dll SHA-256: $versionHash"
AddLine "Known obsolete Full Edition version.dll: $(if ($versionHash -eq '86626E22171A92F719E01C28FD831FF2547D5D9409033460F2311EA20C10428F') {'PRESENT - reinstall to back up'} else {'not present'})"
AddLine "Stock PAK: $(if (Test-Path -LiteralPath (Join-Path $pak 'SummerCamp-WindowsNoEditor.pak')) {'PRESENT'} else {'MISSING'})"
AddLine "Stock SIG: $(if (Test-Path -LiteralPath (Join-Path $pak 'SummerCamp-WindowsNoEditor.sig')) {'PRESENT'} else {'MISSING'})"
$legacyPak = Join-Path $pak 'SummerCamp-WindowsNoEditor_OfflineBots_P.pak'
AddLine "Obsolete Offline Bots menu PAK: $(if (Test-Path -LiteralPath $legacyPak) {'PRESENT - CONFLICT'} else {'not present'})"
$steamRoot = Join-Path ${env:ProgramFiles(x86)} 'Steam'
$shortcutFiles = @(Get-ChildItem -Path (Join-Path $steamRoot 'userdata\*\config\shortcuts.vdf') -ErrorAction SilentlyContinue)
$noPakFound = $false
foreach ($shortcut in $shortcutFiles) {
    $raw = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($shortcut.FullName))
    $index = $raw.IndexOf('SummerCamp-Win64-Shipping.exe', [StringComparison]::OrdinalIgnoreCase)
    if ($index -ge 0 -and $raw.Substring($index, [Math]::Min(700, $raw.Length - $index)) -match '(?i)-NoPak') { $noPakFound = $true }
}
AddLine "Old -NoPak in scanned Steam shortcuts: $(if ($noPakFound) {'DETECTED - remove launch option'} elseif ($shortcutFiles.Count) {'not detected; verify Steam UI if using a different shortcut'} else {'Steam shortcut files unavailable; inspect Steam UI'})"
foreach ($name in @('ResurrectedOfflineBotsLoader.dll','ResurrectedOfflineBotsController.dll')) {
    AddLine "Obsolete $name`: $(if (Test-Path -LiteralPath (Join-Path $bin $name)) {'PRESENT - REVIEW'} else {'not present'})"
}
$log = Join-Path $env:TEMP 'ResurrectedOfflineBots-Bootstrap.log'
if (Test-Path -LiteralPath $log) {
    $item = Get-Item -LiteralPath $log
    AddLine "Bootstrap log: $log (updated $($item.LastWriteTime.ToString('u')))"
    Get-Content -LiteralPath $log -Tail 30 | ForEach-Object { AddLine "  $_" }
    $recent = Get-Content -LiteralPath $log -Tail 30
    if ($recent -match 'selected bootstrap method: application-local X3DAudio1_7.dll proxy' -and $recent -match 'backend LOADED:') {
        AddLine 'BOOTSTRAP: LAST OBSERVED OK (check log timestamp against the current launch)'
        AddLine 'OFFLINE BOTS BACKEND: LAST OBSERVED LOADED'
    } else { AddLine 'BOOTSTRAP: RUNTIME VERIFICATION PENDING OR FAILED' }
} else { AddLine 'BOOTSTRAP: RUNTIME VERIFICATION PENDING - no bootstrap log yet' }
$installedProxyHash = HashOrMissing (Join-Path $bin 'X3DAudio1_7.dll')
$payloadProxyHash = HashOrMissing (Join-Path $PSScriptRoot 'ResurrectedOfflineBots\X3DAudio1_7.dll')
$installedBackendHash = HashOrMissing (Join-Path $bin 'ResurrectedOfflineBots.dll')
$payloadBackendHash = HashOrMissing (Join-Path $PSScriptRoot 'ResurrectedOfflineBots\ResurrectedOfflineBots.dll')
$allFiles = $hash -eq $supported -and
    $installedProxyHash -ne 'MISSING' -and $installedProxyHash -eq $payloadProxyHash -and
    $installedBackendHash -ne 'MISSING' -and $installedBackendHash -eq $payloadBackendHash
AddLine "FINAL STATUS: $(if ($allFiles -and -not (Test-Path -LiteralPath $legacyPak)) {'FILES OK; check runtime evidence and Full menu'} else {'INSTALLATION NEEDS ATTENTION'})"
$lines | Set-Content -LiteralPath $report -Encoding UTF8
Write-Output "Diagnostic saved: $report"
if ($allFiles) { exit 0 } else { exit 1 }
