$ErrorActionPreference = 'Stop'
$supported = '5541268C88B6C02BFB8BDA2D4B07E3E04BB6A03CEF1C5E89163B1E9FBC32A430'
$root = $PSScriptRoot
$bin = Join-Path $root 'SummerCamp\Binaries\Win64'
$exe = Join-Path $bin 'SummerCamp-Win64-Shipping.exe'
$paks = Join-Path $root 'SummerCamp\Content\Paks'
$payload = Join-Path $root 'ResurrectedOfflineBots'
$backup = Join-Path $root 'ResurrectedOfflineBots-CompatibilityFix-Backup'

try {
    if (-not (Test-Path -LiteralPath (Join-Path $root 'SummerCamp.exe'))) { throw 'Extract the package into the game folder containing SummerCamp.exe.' }
    if (-not (Test-Path -LiteralPath $exe)) { throw 'Shipping EXE missing.' }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash
    if ($actual -ne $supported) { throw "UNSUPPORTED GAME BUILD: $actual" }
    foreach ($name in @('SummerCamp-WindowsNoEditor.pak','SummerCamp-WindowsNoEditor.sig')) {
        if (-not (Test-Path -LiteralPath (Join-Path $paks $name))) { throw "Original game file missing: $name" }
    }
    foreach ($name in @('X3DAudio1_7.dll','ResurrectedOfflineBots.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $payload $name))) { throw "Incomplete mod payload: $name" }
    }
    if (Get-Process -Name 'SummerCamp-Win64-Shipping' -ErrorAction SilentlyContinue) { throw 'Close the game before installing.' }
    $steamRoot = Join-Path ${env:ProgramFiles(x86)} 'Steam'
    foreach ($shortcut in @(Get-ChildItem -Path (Join-Path $steamRoot 'userdata\*\config\shortcuts.vdf') -ErrorAction SilentlyContinue)) {
        $raw = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($shortcut.FullName))
        $index = $raw.IndexOf('SummerCamp-Win64-Shipping.exe', [StringComparison]::OrdinalIgnoreCase)
        if ($index -ge 0 -and $raw.Substring($index, [Math]::Min(700, $raw.Length - $index)) -match '(?i)-NoPak') {
            throw 'An old -NoPak option was detected in a Steam shortcut. Remove that option in Steam before installing.'
        }
    }
    New-Item -ItemType Directory -Path $backup -Force | Out-Null
    $oldVersion = Join-Path $bin 'version.dll'
    $oldVersionBackup = Join-Path $backup 'version.dll.frozen-full-edition'
    if (Test-Path -LiteralPath $oldVersion) {
        $versionHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $oldVersion).Hash
        if ($versionHash -eq '86626E22171A92F719E01C28FD831FF2547D5D9409033460F2311EA20C10428F') {
            if (Test-Path -LiteralPath $oldVersionBackup) { throw 'Older version.dll backup already exists; review it before reinstalling.' }
            Move-Item -LiteralPath $oldVersion -Destination $oldVersionBackup
            Write-Output 'Backed up the known obsolete Full Edition version.dll proxy.'
        } else {
            Write-Warning "Existing version.dll has a different SHA-256 ($versionHash); left untouched because it may belong to another mod."
        }
    }
    # Only a file with this exact name belongs to the older Offline Bots menu.
    # Never alter the original pak or sig, unrelated mods, saves, or Steam files.
    $legacyPak = Join-Path $paks 'SummerCamp-WindowsNoEditor_OfflineBots_P.pak'
    if (Test-Path -LiteralPath $legacyPak) {
        $legacyBackup = Join-Path $backup 'SummerCamp-WindowsNoEditor_OfflineBots_P.pak'
        if (Test-Path -LiteralPath $legacyBackup) { throw 'Existing legacy PAK backup needs review; no files were replaced.' }
        Move-Item -LiteralPath $legacyPak -Destination $legacyBackup
        Write-Output 'Backed up obsolete Offline Bots menu PAK.'
    }
    foreach ($name in @('X3DAudio1_7.dll','ResurrectedOfflineBots.dll')) {
        $target = Join-Path $bin $name
        $source = Join-Path $payload $name
        $original = Join-Path $backup ($name + '.original')
        $marker = Join-Path $backup ($name + '.no-original')
        if (-not (Test-Path -LiteralPath $original) -and -not (Test-Path -LiteralPath $marker)) {
            if (Test-Path -LiteralPath $target) { Copy-Item -LiteralPath $target -Destination $original }
            else { New-Item -ItemType File -Path $marker | Out-Null }
        }
        Copy-Item -LiteralPath $source -Destination $target -Force
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash -ne (Get-FileHash -Algorithm SHA256 -LiteralPath $target).Hash) {
            throw "Copy verification failed: $name"
        }
    }
    Write-Output 'FILES INSTALLED SUCCESSFULLY'
    Write-Output 'BOOTSTRAP FILES VERIFIED: EXE directly imports X3DAudio1_7.dll; application-local proxy installed.'
    Write-Output 'RUNTIME VERIFICATION PENDING: launch through Steam, then run VERIFY-OFFLINE-BOTS-INSTALL.bat.'
    if (Test-Path -LiteralPath $oldVersion) { Write-Output 'NOTE: unrelated version.dll remains; the new bootstrap does not depend on it.' }
    Write-Output 'No EXE, original pak/sig, Steam files, or saves were changed.'
    exit 0
} catch {
    Write-Error "INSTALL FAILED: $($_.Exception.Message)"
    exit 1
}
