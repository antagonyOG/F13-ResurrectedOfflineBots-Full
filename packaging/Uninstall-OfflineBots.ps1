$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$bin = Join-Path $root 'SummerCamp\Binaries\Win64'
$backup = Join-Path $root 'ResurrectedOfflineBots-CompatibilityFix-Backup'
if (Get-Process -Name 'SummerCamp-Win64-Shipping' -ErrorAction SilentlyContinue) { throw 'Close the game before uninstalling.' }
foreach ($name in @('X3DAudio1_7.dll','ResurrectedOfflineBots.dll')) {
    $target = Join-Path $bin $name
    $source = Join-Path $PSScriptRoot "ResurrectedOfflineBots\$name"
    $original = Join-Path $backup ($name + '.original')
    $marker = Join-Path $backup ($name + '.no-original')
    if (-not (Test-Path -LiteralPath $original) -and -not (Test-Path -LiteralPath $marker)) { continue }
    if (Test-Path -LiteralPath $target) {
        if (-not (Test-Path -LiteralPath $source) -or
            (Get-FileHash -Algorithm SHA256 -LiteralPath $target).Hash -ne (Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash) {
            Write-Warning "Changed file left untouched: $target"
            continue
        }
    }
    if (Test-Path -LiteralPath $original) { Copy-Item -LiteralPath $original -Destination $target -Force }
    elseif (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target }
    Write-Output "Restored $name"
}
$legacyBackup = Join-Path $backup 'SummerCamp-WindowsNoEditor_OfflineBots_P.pak'
$legacyTarget = Join-Path $root 'SummerCamp\Content\Paks\SummerCamp-WindowsNoEditor_OfflineBots_P.pak'
if (Test-Path -LiteralPath $legacyBackup) {
    if (Test-Path -LiteralPath $legacyTarget) { Write-Warning "Legacy PAK target already exists; backup retained: $legacyBackup" }
    else { Copy-Item -LiteralPath $legacyBackup -Destination $legacyTarget; Write-Output 'Restored older Offline Bots menu PAK; remove it before using this packed release again.' }
}
$oldVersionBackup = Join-Path $backup 'version.dll.frozen-full-edition'
$oldVersionTarget = Join-Path $bin 'version.dll'
if (Test-Path -LiteralPath $oldVersionBackup) {
    if (Test-Path -LiteralPath $oldVersionTarget) { Write-Warning "version.dll target exists; older proxy backup retained: $oldVersionBackup" }
    else { Copy-Item -LiteralPath $oldVersionBackup -Destination $oldVersionTarget; Write-Output 'Restored prior Full Edition version.dll.' }
}
Write-Output 'Original EXE, pak/sig, Steam files, and saves were untouched.'
