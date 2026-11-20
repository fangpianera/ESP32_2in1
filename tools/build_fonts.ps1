<#
.SYNOPSIS
  生成 EVA-Nav 的中文字体（GB2312 全字库，常驻固件）与中心区数字字体
.DESCRIPTION
  历史教训（2026-11-19 板端实拍）：
  1) 早先只收 ~500 个"导航高频字"，目的地名/路名一出现生僻字就渲染成方块（口口口），
     所以改为整库内嵌 GB2312（7614 字 + ASCII + 全角/符号区）。
  2) 中心区"路线"行曾用"同文本 + 偏 1px 的第二个 label"做 faux bold，板端分块刷新时
     两份字错开写入 → 字叠字花屏。现改为单 label + 字面自带重量（SimHei）。
  3) 2026-11-19 用户要求"文字块整体放大并下移一点"：不采用 LVGL 缩放（位图拉伸会糊），
     改为重新生成更大字号的字库 —— 目的地/剩余 18→20px（Noto Sans SC），
     路线 24→28px（SimHei，粗面），速度数字 48→56px（Chakra Petch SemiBold）。
     app 分区 5 MB，旧字号 cjk18/cjk24/eva_tech_48 已移出构建（tools/fonts_history/）。

  字体源：Noto Sans SC（OFL，可商用）；SimHei 取 Windows 自带；Chakra Petch 在 tools/fonts/。
.PARAMETER Font
  中文正文字体 TTF（默认 Windows 自带 NotoSansSC-VF.ttf）
.PARAMETER BoldFont
  中文粗面 TTF（默认 Windows 自带 simhei.ttf）
.PARAMETER TechFont
  机甲数字 TTF（默认 tools/fonts/ChakraPetch-SemiBold.ttf）
.PARAMETER OutRoot
  工程根（默认脚本所在 tools 的上一级）
#>
param(
    [string]$Font     = "C:\Windows\Fonts\NotoSansSC-VF.ttf",
    [string]$BoldFont = "C:\Windows\Fonts\simhei.ttf",
    [string]$TechFont = "$PSScriptRoot\fonts\ChakraPetch-SemiBold.ttf",
    [string]$OutRoot  = "$PSScriptRoot\.."
)

$ErrorActionPreference = "Stop"
foreach ($p in @($Font, $BoldFont, $TechFont)) {
    if (-not (Test-Path $p)) { throw "字体文件不存在：$p" }
}

$conv    = Join-Path $PSScriptRoot "fonts\node_modules\lv_font_conv\lv_font_conv.js"
$nodeBin = "D:\Huawei\DevEcoStudio\tools\node\node.exe"
if (-not (Test-Path $nodeBin)) { $nodeBin = "node" }

$symbolFile = Join-Path $PSScriptRoot "gb2312_symbols.txt"
if (-not (Test-Path $symbolFile)) { throw "缺少字表 $symbolFile（GB2312 全字集，单行字符）" }
$symbols = (Get-Content $symbolFile -Raw).TrimEnd("`r", "`n")
Write-Host "[font] GB2312 字符数 = $($symbols.Length)" -ForegroundColor Cyan

$mainFonts = Join-Path $OutRoot "firmware\main\fonts"
New-Item -ItemType Directory -Force -Path $mainFonts | Out-Null

function Build-CJK([string]$ttf, [int]$px, [string]$name) {
    Write-Host "[font] $name (${px}px, $ttf)" -ForegroundColor Cyan
    & $nodeBin $conv `
        --font $ttf `
        -r "0x20-0x7E" --symbols $symbols `
        --size $px --bpp 4 --no-kerning `
        --format lvgl --lv-include "lvgl.h" --lv-font-name $name `
        -o (Join-Path $mainFonts "$name.c")
    if ($LASTEXITCODE -ne 0) { throw "lv_font_conv 失败（$name, exit=$LASTEXITCODE）" }
}

# 目的地 / 剩余里程：20px 正文字面
Build-CJK $Font 20 "cjk20"
# 当前前进路线：28px 粗面（单 label，用字重代替 faux bold）
Build-CJK $BoldFont 28 "cjk28"

# 中心速度数字：56px 机甲字面（ASCII + 度数符号，供 m/s、温度等处复用）
Write-Host "[font] eva_tech_56 (56px, Chakra Petch)" -ForegroundColor Cyan
& $nodeBin $conv `
    --font $TechFont `
    -r "0x20-0x7E" -r "0xB0" `
    --size 56 --bpp 4 --format lvgl --no-kerning --no-compress `
    --lv-include "lvgl.h" --lv-font-name "eva_tech_56" `
    -o (Join-Path $mainFonts "eva_tech_56.c")
if ($LASTEXITCODE -ne 0) { throw "lv_font_conv 失败（eva_tech_56, exit=$LASTEXITCODE）" }

Get-ChildItem $mainFonts -File | Select-Object Name, @{n='KB';e={[math]::Round($_.Length/1KB,1)}} | Format-Table
Write-Host "完成：cjk20.c / cjk28.c / eva_tech_56.c 已生成，CMakeLists.txt 已引用，直接 idf.py build 即可。" -ForegroundColor Green
