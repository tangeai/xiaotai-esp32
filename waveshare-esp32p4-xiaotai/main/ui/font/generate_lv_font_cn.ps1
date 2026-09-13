param(
    [string]$FontPath = 'C:/Windows/Fonts/simhei.ttf',
    [string]$CoverageFontPath = 'C:/Windows/Fonts/NotoSansSC-VF.ttf',
    [string]$EmojiFontPath = 'C:/Windows/Fonts/seguiemj.ttf',
    [string]$KaomojiFontPath = 'C:/Windows/Fonts/seguisym.ttf',
    [int[]]$Sizes = @(14),
    [int]$MaxChars = 9000,
    [int]$FallbackMaxChars = 900,
    [switch]$GenerateSpiffsBinary
)

$ErrorActionPreference = 'Stop'

function Add-UniqueChars {
    param(
        [System.Collections.Generic.List[string]]$Target,
        [System.Collections.Generic.HashSet[string]]$Seen,
        [string]$Text,
        [int]$Limit = 0
    )

    if ([string]::IsNullOrEmpty($Text)) {
        return
    }

    foreach ($char in $Text.ToCharArray()) {
        $value = [string]$char
        if ([char]::IsWhiteSpace($char) -or [int][char]$char -eq 0xfeff) {
            continue
        }
        if ($Seen.Add($value)) {
            $Target.Add($value)
            if ($Limit -gt 0 -and $Target.Count -ge $Limit) {
                return
            }
        }
    }
}

function Join-Codepoints {
    param([int[]]$Codepoints)

    $builder = [System.Text.StringBuilder]::new()
    foreach ($codepoint in $Codepoints) {
        [void]$builder.Append([char]$codepoint)
    }
    return $builder.ToString()
}

function Read-TextFile {
    param([string]$Path)

    if (!(Test-Path $Path)) {
        return ''
    }
    return Get-Content $Path -Raw -Encoding UTF8
}

function Get-CodepointRangeCount {
    param([string]$Range)

    $parts = $Range -split '-'
    if ($parts.Count -eq 1) {
        return 1
    }

    $start = [Convert]::ToInt32($parts[0].Replace('0x', ''), 16)
    $end = [Convert]::ToInt32($parts[1].Replace('0x', ''), 16)
    return ($end - $start + 1)
}

function Get-PartitionSize {
    param(
        [string]$CsvPath,
        [string]$PartitionName
    )

    if (!(Test-Path $CsvPath)) {
        return 0
    }

    foreach ($line in Get-Content $CsvPath -Encoding UTF8) {
        $trimmed = $line.Trim()
        if ([string]::IsNullOrEmpty($trimmed) -or $trimmed.StartsWith('#')) {
            continue
        }
        $columns = $trimmed -split ','
        if ($columns.Count -lt 5) {
            continue
        }
        if ($columns[0].Trim() -ne $PartitionName) {
            continue
        }
        $rawSize = $columns[4].Trim()
        if ($rawSize.StartsWith('0x')) {
            return [Convert]::ToInt32($rawSize.Substring(2), 16)
        }
        return [int]$rawSize
    }

    return 0
}

function Resolve-FontPath {
    param(
        [string]$RequestedPath,
        [string[]]$FallbackPaths,
        [string]$Label,
        [bool]$Required = $true
    )

    if (![string]::IsNullOrEmpty($RequestedPath) -and (Test-Path $RequestedPath)) {
        return $RequestedPath
    }

    foreach ($path in $FallbackPaths) {
        if (Test-Path $path) {
            Write-Warning "$Label font not found at '$RequestedPath', using '$path'."
            return $path
        }
    }

    if ($Required) {
        throw "No usable $Label font file found."
    }

    Write-Warning "$Label font not found; related glyph ranges will be skipped."
    return ''
}

if ($Sizes.Count -ne 1) {
    throw 'AI chat runtime text must use exactly one font size.'
}

if (!(Test-Path $FontPath)) {
    $FontPath = Resolve-FontPath `
        -RequestedPath $FontPath `
        -FallbackPaths @('C:/Windows/Fonts/NotoSansSC-VF.ttf', 'C:/Windows/Fonts/msyh.ttc') `
        -Label 'AI chat text' `
        -Required $true
}

$EmojiFontPath = Resolve-FontPath `
    -RequestedPath $EmojiFontPath `
    -FallbackPaths @('C:/Windows/Fonts/seguisym.ttf') `
    -Label 'AI chat emoji' `
    -Required $false

$KaomojiFontPath = Resolve-FontPath `
    -RequestedPath $KaomojiFontPath `
    -FallbackPaths @('C:/Windows/Fonts/seguiemj.ttf', 'C:/Windows/Fonts/NotoSansSC-VF.ttf') `
    -Label 'AI chat kaomoji' `
    -Required $false

$CoverageFontPath = Resolve-FontPath `
    -RequestedPath $CoverageFontPath `
    -FallbackPaths @('C:/Windows/Fonts/msyh.ttc') `
    -Label 'AI chat coverage' `
    -Required $false

$textFontRanges = @(
    '0x20-0x7f',
    '0xa0-0xff',
    '0x2000-0x206f',
    '0x20a0-0x20cf',
    '0x2100-0x214f',
    '0x2190-0x21ff',
    '0x2200-0x22ff',
    '0x2300-0x23ff',
    '0x2460-0x24ff',
    '0x2500-0x257f',
    '0x25a0-0x25ff',
    '0x2e80-0x2eff',
    '0x3000-0x303f',
    '0x3040-0x309f',
    '0x30a0-0x30ff',
    '0x3100-0x312f',
    '0x3200-0x32ff',
    '0x3300-0x33ff',
    '0x3400-0x4dbf',
    '0x4e00-0x9fff',
    '0xf900-0xfaff',
    '0xfe10-0xfe1f',
    '0xfe30-0xfe4f',
    '0xff00-0xffef',
    '0x20000-0x2a6df',
    '0x2a700-0x2b73f',
    '0x2b740-0x2b81f',
    '0x2b820-0x2ceaf',
    '0x2ceb0-0x2ebef',
    '0x30000-0x3134f'
)

$emojiFontRanges = @(
    '0x2600-0x26ff',
    '0x2700-0x27bf',
    '0x1f000-0x1f02f',
    '0x1f0a0-0x1f0ff',
    '0x1f100-0x1f1ff',
    '0x1f300-0x1f5ff',
    '0x1f600-0x1f64f',
    '0x1f680-0x1f6ff',
    '0x1f780-0x1f7ff',
    '0x1f900-0x1f9ff',
    '0x1fa70-0x1faff'
)

if ([string]::IsNullOrEmpty($EmojiFontPath)) {
    $emojiFontRanges = @()
}

$coverageTextFontRanges = @(
    '0xfe10-0xfe1f',
    '0x20000-0x2a6df',
    '0x2a700-0x2b73f',
    '0x2b740-0x2b81f',
    '0x2b820-0x2ceaf',
    '0x2ceb0-0x2ebef',
    '0x30000-0x3134f'
)

$fontDir = $PSScriptRoot
$mainRoot = Split-Path -Parent (Split-Path -Parent $fontDir)
$projectRoot = Split-Path -Parent $mainRoot
$charsetRoot = Join-Path $projectRoot 'assets/font/charset'
$generatedRoot = Join-Path $projectRoot 'assets/font/generated'
$toolRoot = Join-Path $mainRoot '.fonttool'
$fontConv = Join-Path $toolRoot 'node_modules/.bin/lv_font_conv.cmd'
$fontConvArgs = @()

if (!(Test-Path $fontConv)) {
    $fontConv = 'npx'
    $fontConvArgs = @('-y', 'lv_font_conv')
}

New-Item -ItemType Directory -Force $generatedRoot | Out-Null
if ($GenerateSpiffsBinary) {
    $spiffsRoot = Join-Path $projectRoot 'assets/font/spiffs'
    New-Item -ItemType Directory -Force $spiffsRoot | Out-Null
    Get-ChildItem -LiteralPath $spiffsRoot -Filter 'lv_font_cn_*.bin' -ErrorAction SilentlyContinue |
        Remove-Item -Force
} else {
    $spiffsRoot = ''
}
Get-ChildItem -LiteralPath $fontDir -Filter 'lv_font_cn_*.c' -ErrorAction SilentlyContinue |
    Remove-Item -Force

$aiSourceText = ''
foreach ($path in @(
    (Join-Path $mainRoot 'services/ai_chat.c'),
    (Join-Path $mainRoot 'services/ai_chat_events.c'),
    (Join-Path $mainRoot 'services/ai_chat_token.c')
)) {
    $aiSourceText += Read-TextFile $path
}

$punctuationText = Read-TextFile (Join-Path $charsetRoot '00_punctuation.txt')
$kaomojiText = Read-TextFile (Join-Path $charsetRoot '01_kaomoji_symbols.txt')
$fallbackText = Read-TextFile (Join-Path $charsetRoot '05_fallback_terms.txt')
$aiUiText = Read-TextFile (Join-Path $charsetRoot '15_ai_chat_ui_terms.txt')
$domainText = Read-TextFile (Join-Path $charsetRoot '20_ai_chat_domain_terms.txt')
$commonText = Read-TextFile (Join-Path $charsetRoot '90_common_compact.txt')
$common3500Text = Read-TextFile (Join-Path $charsetRoot '95_common_3500.txt')
$common7000Text = Read-TextFile (Join-Path $charsetRoot '96_common_7000.txt')
$standard8105Text = Read-TextFile (Join-Path $charsetRoot '97_standard_8105.txt')

$fullChars = [System.Collections.Generic.List[string]]::new()
$fullSeen = [System.Collections.Generic.HashSet[string]]::new()
Add-UniqueChars $fullChars $fullSeen $punctuationText
Add-UniqueChars $fullChars $fullSeen $kaomojiText
Add-UniqueChars $fullChars $fullSeen $aiSourceText
Add-UniqueChars $fullChars $fullSeen $domainText
Add-UniqueChars $fullChars $fullSeen $commonText $MaxChars
Add-UniqueChars $fullChars $fullSeen $common3500Text $MaxChars
Add-UniqueChars $fullChars $fullSeen $common7000Text $MaxChars
Add-UniqueChars $fullChars $fullSeen $standard8105Text $MaxChars

$fallbackChars = [System.Collections.Generic.List[string]]::new()
$fallbackSeen = [System.Collections.Generic.HashSet[string]]::new()
Add-UniqueChars $fallbackChars $fallbackSeen $punctuationText
Add-UniqueChars $fallbackChars $fallbackSeen $kaomojiText $FallbackMaxChars
Add-UniqueChars $fallbackChars $fallbackSeen $aiUiText $FallbackMaxChars
Add-UniqueChars $fallbackChars $fallbackSeen $domainText $FallbackMaxChars
Add-UniqueChars $fallbackChars $fallbackSeen $aiSourceText $FallbackMaxChars
Add-UniqueChars $fallbackChars $fallbackSeen $fallbackText $FallbackMaxChars

$fullSymbols = -join $fullChars
$fallbackSymbols = -join $fallbackChars
$fallbackFontSymbols = -join ($fallbackChars | Where-Object { [int][char]$_ -gt 0x7f })
$kaomojiChars = [System.Collections.Generic.List[string]]::new()
$kaomojiSeen = [System.Collections.Generic.HashSet[string]]::new()
Add-UniqueChars $kaomojiChars $kaomojiSeen $kaomojiText
$kaomojiSymbols = -join $kaomojiChars
$kaomojiFontGroups = @(
    @{ Path = $KaomojiFontPath; Symbols = $kaomojiSymbols },
    @{ Path = 'C:/Windows/Fonts/arial.ttf'; Symbols = (Join-Codepoints @(0x0648, 0x0669, 0x1d17, 0x1d25, 0x1d54, 0x1d55)) },
    @{ Path = 'C:/Windows/Fonts/LeelawUI.ttf'; Symbols = (Join-Codepoints @(0x0e05, 0x0e07, 0x0e51, 0x0eb6)) },
    @{ Path = 'C:/Windows/Fonts/himalaya.ttf'; Symbols = (Join-Codepoints @(0x0f0d, 0x0f0e)) },
    @{ Path = 'C:/Windows/Fonts/calibri.ttf'; Symbols = (Join-Codepoints @(0x10da)) },
    @{ Path = 'C:/Windows/Fonts/malgun.ttf'; Symbols = (Join-Codepoints @(0x1107, 0x1109, 0x11ba, 0x11bd, 0xb208)) },
    @{ Path = 'C:/Windows/Fonts/gadugi.ttf'; Symbols = (Join-Codepoints @(0x141b, 0x141f, 0x1420, 0x1452, 0x146d, 0x1555, 0x1559, 0x15d2, 0x15d5, 0x15dc, 0x15e3, 0x15e8)) }
)
$standardExtraSeen = [System.Collections.Generic.HashSet[string]]::new()
$standardExtraChars = [System.Collections.Generic.List[string]]::new()
foreach ($char in $standard8105Text.ToCharArray()) {
    $codepoint = [int][char]$char
    if ($codepoint -le 0x7f -or $codepoint -eq 0xfeff -or $codepoint -eq 0xfffd) {
        continue
    }
    if ($codepoint -ge 0xd800 -and $codepoint -le 0xdfff) {
        continue
    }
    if ($codepoint -ge 0x4e00 -and $codepoint -le 0x9fff) {
        continue
    }
    if ($standardExtraSeen.Add([string]$char)) {
        $standardExtraChars.Add([string]$char)
    }
}
$standardExtraSymbols = -join $standardExtraChars
$textRangeChars = 0
foreach ($range in $textFontRanges) {
    $textRangeChars += Get-CodepointRangeCount $range
}
$emojiRangeChars = 0
foreach ($range in $emojiFontRanges) {
    $emojiRangeChars += Get-CodepointRangeCount $range
}
$externalFontChars = $textRangeChars + $emojiRangeChars + $standardExtraChars.Count

$fullCharsPath = Join-Path $generatedRoot 'lv_font_cn_chars.txt'
$fallbackCharsPath = Join-Path $generatedRoot 'lv_font_cn_fallback_chars.txt'
$reportPath = Join-Path $generatedRoot 'font_charset_report.txt'
Set-Content -Path $fullCharsPath -Value $fullSymbols -Encoding UTF8
Set-Content -Path $fallbackCharsPath -Value $fallbackSymbols -Encoding UTF8

Push-Location $fontDir
try {
    foreach ($size in $Sizes) {
        $cOutFile = "lv_font_cn_$size.c"
        $externalFontArgs = @('--font', $FontPath)
        foreach ($range in $textFontRanges) {
            if ($coverageTextFontRanges -notcontains $range) {
                $externalFontArgs += @('-r', $range)
            }
        }
        if (![string]::IsNullOrEmpty($CoverageFontPath)) {
            $externalFontArgs += @('--font', $CoverageFontPath)
            foreach ($range in $coverageTextFontRanges) {
                $externalFontArgs += @('-r', $range)
            }
        }
        if (![string]::IsNullOrEmpty($standardExtraSymbols)) {
            $externalFontArgs += @('--symbols', $standardExtraSymbols)
        }
        if (![string]::IsNullOrEmpty($EmojiFontPath)) {
            $externalFontArgs += @('--font', $EmojiFontPath)
            foreach ($range in $emojiFontRanges) {
                $externalFontArgs += @('-r', $range)
            }
        }
        foreach ($group in $kaomojiFontGroups) {
            if (![string]::IsNullOrEmpty($group.Path) -and
                (Test-Path $group.Path) -and
                ![string]::IsNullOrEmpty($group.Symbols)) {
                $externalFontArgs += @('--font', $group.Path, '--symbols', $group.Symbols)
            }
        }

        & $fontConv @fontConvArgs --bpp 4 --size $size @externalFontArgs --format lvgl -o $cOutFile --force-fast-kern-format --no-compress
        if ($LASTEXITCODE -ne 0) {
            throw "lv_font_conv failed while generating $cOutFile"
        }

        if ($GenerateSpiffsBinary) {
            $binOutFile = Join-Path $spiffsRoot "lv_font_cn_$size.bin"
            & $fontConv @fontConvArgs --bpp 4 --size $size @externalFontArgs --format bin -o $binOutFile --force-fast-kern-format --no-compress
            if ($LASTEXITCODE -ne 0) {
                throw "lv_font_conv failed while generating $binOutFile"
            }
        }
    }
} finally {
    Pop-Location
}

$report = @()
$report += "font=$FontPath"
$report += "coverage_font=$CoverageFontPath"
$report += "emoji_font=$EmojiFontPath"
$report += "kaomoji_font=$KaomojiFontPath"
$report += "runtime_font_sizes=$($Sizes -join ',')"
$report += "full_chars=$($fullChars.Count)"
$report += "fallback_chars=$($fallbackChars.Count)"
$report += "max_chars=$MaxChars"
$report += "fallback_max_chars=$FallbackMaxChars"
$report += "external_font_chars=$externalFontChars"
$report += "font_storage=compiled_const_flash"
$report += "spiffs_binary_generated=$([bool]$GenerateSpiffsBinary)"
$report += "external_text_font_ranges=$($textFontRanges -join ',')"
$report += "external_emoji_font_ranges=$($emojiFontRanges -join ',')"
$report += "external_text_range_chars=$textRangeChars"
$report += "external_emoji_range_chars=$emojiRangeChars"
$report += "external_font_extra_chars=$($standardExtraChars.Count)"
$report += "kaomoji_chars=$($kaomojiChars.Count)"
$report += "kaomoji_extra_fonts=$(($kaomojiFontGroups | Where-Object { (Test-Path $_.Path) -and ![string]::IsNullOrEmpty($_.Symbols) } | ForEach-Object { $_.Path }) -join ',')"
$report += "usage=ai_chat_only"
$report += "external_font_compressed=false"
foreach ($size in $Sizes) {
    $cFile = Join-Path $fontDir "lv_font_cn_$size.c"
    if (Test-Path $cFile) {
        $report += "lv_font_cn_$size.c=$((Get-Item $cFile).Length)"
    }
}
if ($GenerateSpiffsBinary) {
    $fontFiles = Get-ChildItem $spiffsRoot -Filter 'lv_font_cn_*.bin' | Sort-Object Name
    $fontBytes = 0
    foreach ($file in $fontFiles) {
        $fontBytes += $file.Length
    }
    $fontsPartitionSize = Get-PartitionSize (Join-Path $projectRoot 'partitions.csv') 'fonts'
    if ($fontsPartitionSize -gt 0) {
        $report += "fonts_partition_size=$fontsPartitionSize"
        $report += "fonts_partition_used=$fontBytes"
        $report += "fonts_partition_free=$($fontsPartitionSize - $fontBytes)"
    }
    foreach ($file in $fontFiles) {
        $report += "$($file.Name)=$($file.Length)"
    }
}
Set-Content -Path $reportPath -Value ($report -join "`r`n") -Encoding UTF8

$report -join "`n"
