param(
    [ValidateSet('build','flash','app-flash','monitor','menuconfig','fullclean')]
    [string]$Action='build',
    [string]$Port
)
$ErrorActionPreference='Stop'
if (!$env:IDF_PATH) { throw 'Open an ESP-IDF 5.5.5 terminal or run ESP-IDF export.ps1 first.' }
Push-Location $PSScriptRoot
try {
    $idfArgs=@("$env:IDF_PATH/tools/idf.py")
    if ($Port) {$idfArgs+=@('-p',$Port)}
    if ($Action -in @('flash','app-flash','monitor') -and !$Port) {throw 'Specify -Port COMx (or the Linux serial device).'}
    $idfArgs+=$Action
    & python @idfArgs
    exit $LASTEXITCODE
} finally {Pop-Location}
