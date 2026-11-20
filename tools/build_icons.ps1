<#
.SYNOPSIS
  EVA 机甲风 SVG 图标 → LVGL 96x96 .bin
.DESCRIPTION
  流程：Inkscape(或 rsvg-convert) 先导出 96x96 透明 PNG，再 lv_img_conv 转 bin。
  为什么用 bin 而不是 .c：.c 会进 .rodata 常驻 flash+映射，十几个 96x96 图标
  就吃掉 400KB+；bin 放 LittleFS 可以按场景加载/卸载（doc §8 资源预算）。

  图标清单（按 UI 实际用到的来，别多转）：
    arrow     导航箭头主标（非 UI 用，保留给开机动画）
    turn_left / turn_right / turn_left_hard / turn_right_hard
    uturn / straight / arrive
    off_route  偏航警示
    gps_ok / gps_lost
#>
param(
    [string]$SvgDir = "$PSScriptRoot\..\assets\icons",
    [string]$OutDir = "$PSScriptRoot\..\storage\icon",
    [int]$Size = 96
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# 找一个可用的 SVG 光栅化工具
$raster = $null
foreach ($cand in @("inkscape", "rsvg-convert", "magick")) {
    $p = Get-Command $cand -ErrorAction SilentlyContinue
    if ($p) { $raster = $p.Source; break }
}
if (-not $raster) {
    Write-Warning "未找到 inkscape/rsvg-convert/imagemagick。请先安装其一（winget install Inkscape.Inkscape）"
    Write-Warning "或直接把设计师导出的 ${Size}x${Size} PNG 放到 $OutDir\_png\ 后重跑本脚本"
}

$pngDir = Join-Path $OutDir "_png"
New-Item -ItemType Directory -Force -Path $pngDir | Out-Null

Get-ChildItem $SvgDir -Filter *.svg -File | ForEach-Object {
    $base = $_.BaseName
    $png = Join-Path $pngDir "$base.png"
    if ($raster -like "*inkscape*") {
        & $raster $_.FullName --export-type=png --export-filename=$png --export-width=$Size --export-height=$Size --export-background-opacity=0
    } elseif ($raster -like "*rsvg*") {
        & $raster -w $Size -h $Size -o $png $_.FullName
    } else {
        & $raster $_.FullName -resize "${Size}x${Size}" -background none $png
    }
    if (-not (Test-Path $png)) { Write-Warning "光栅化失败：$($_.Name)"; return }

    # LVGL bin：true_color_alpha = ARGB8888，96x96 约 36KB/个
    $bin = Join-Path $OutDir ("{0}{1}.bin" -f $base, $Size)
    & npx --yes lv_img_conv --format bin -f true_color_alpha -o $bin $png
    if ($LASTEXITCODE -ne 0) { Write-Warning "lv_img_conv 失败：$base" }
}

Get-ChildItem $OutDir -Filter *.bin | Select-Object Name, @{n='KB';e={[math]::Round($_.Length/1KB,1)}} | Format-Table
Write-Host "约定：文件名 = <name><size>.bin，固件侧 res_icon_create(parent, `"turn_left`", 96) 会拼出 F:/icon/turn_left96.bin" -ForegroundColor Green
