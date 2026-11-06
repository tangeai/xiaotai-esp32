param(
    [Parameter(Mandatory = $true)]
    [string]$BrowserPath
)

$ErrorActionPreference = 'Stop'
$browser = (Resolve-Path -LiteralPath $BrowserPath).Path
$previousBrowser = $env:PUPPETEER_EXECUTABLE_PATH
$previousSkip = $env:PUPPETEER_SKIP_DOWNLOAD
try {
    $env:PUPPETEER_EXECUTABLE_PATH = $browser
    $env:PUPPETEER_SKIP_DOWNLOAD = 'true'
    foreach ($source in Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.mmd' | Sort-Object Name) {
        $output = [System.IO.Path]::ChangeExtension($source.FullName, '.png')
        & npx.cmd --yes --package '@mermaid-js/mermaid-cli@12.2.0' mmdc `
            -i $source.FullName -o $output `
            -c (Join-Path $PSScriptRoot 'mermaid-config.json') `
            -b white -s 2
        if ($LASTEXITCODE -ne 0) { throw "Diagram render failed: $($source.Name)" }
    }
} finally {
    $env:PUPPETEER_EXECUTABLE_PATH = $previousBrowser
    $env:PUPPETEER_SKIP_DOWNLOAD = $previousSkip
}
