param([string]$IdfPath="C:\esp\v5.5.5\esp-idf",
      [string]$ToolsPath="C:\Espressif",
      [string]$PythonEnv="",
      [string]$BuildDirectory="build",
      [string]$SdkLibrary="",
      [string]$SdkConfig="")
$ErrorActionPreference="Stop"
$Project=Split-Path -Parent $PSScriptRoot
if (-not (Test-Path -LiteralPath "$IdfPath\tools\idf.py")) { throw "ESP-IDF not found: $IdfPath" }
$env:IDF_TOOLS_PATH=$ToolsPath
$env:IDF_PATH=$IdfPath
if ($PythonEnv) {
    $env:IDF_PYTHON_ENV_PATH=$PythonEnv
    $env:Path="$PythonEnv\Scripts;$env:Path"
}
$env:CCACHE_DISABLE="1"
. "$IdfPath\export.ps1"
Push-Location $Project
try {
    if (-not $SdkLibrary) { $SdkLibrary = "$Project\components\tirtc_sdk\lib\libTiRTC.a" }
    $SdkLibrary = (Resolve-Path -LiteralPath $SdkLibrary).Path.Replace('\', '/')
    $ConfigArguments = @()
    if ($SdkConfig) {
        $SdkConfig = (Resolve-Path -LiteralPath $SdkConfig).Path.Replace('\', '/')
        $ConfigArguments += "-DSDKCONFIG=$SdkConfig"
    } else {
        $ConfigArguments += "-DSDKCONFIG=$($Project.Replace('\', '/'))/sdkconfig"
    }
    python "$IdfPath\tools\idf.py" --no-ccache -B $BuildDirectory "-DTIRTC_C3_LIBRARY=$SdkLibrary" @ConfigArguments build size
    if ($LASTEXITCODE -ne 0) { throw "Build or size analysis failed" }
} finally { Pop-Location }
