param([Parameter(Mandatory=$true)][string]$WorkDir)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech
$config = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'guidance.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$work = [IO.Path]::GetFullPath($WorkDir)
New-Item -ItemType Directory -Path $work -Force | Out-Null
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
try {
    $synth.SelectVoice($config.voice)
    $synth.Rate = $config.rate
    foreach($clip in $config.clips) {
        $path = Join-Path $work ($clip.name + '.wav')
        $synth.SetOutputToWaveFile($path)
        $synth.Speak($clip.text)
        $synth.SetOutputToNull()
        Write-Output $clip.name
    }
} finally { $synth.Dispose() }
