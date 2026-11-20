# =============================================================================
# flash_all.ps1 — EVA 双程序一键烧录（选择器 + 导航 + F1 看板）
#
# 用法：
#   .\flash_all.ps1                 # 默认 COM5
#   .\flash_all.ps1 -Port COM7      # 指定端口
#   .\flash_all.ps1 -SkipF1         # 只烧 选择器 + 导航（F1 槽留空）
#   .\flash_all.ps1 -Only nav       # 只重烧导航这一片（日常改导航用）
#
# 关键约定：
#   * 必须用本目录的分区表：app 槽偏移见下方 $SLOTS，改动会整体错位。
#   * -Only 模式不要带 bootloader/分区表，否则可能把别的槽写坏。
#   * 不要用 `idf.py flash`：它会把 app 默认烧到 0x20000（app_sel，3 MB），
#     而导航镜像 3.67 MB 塞不进去，会溢出写坏 app_nav。
# =============================================================================

param(
    [string]$Port = "COM5",
    [ValidateSet("all", "sel", "nav", "f1")][string]$Only = "all",
    [switch]$SkipF1,
    [int]$Baud = 460800
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Py   = "C:\Users\Administrator\AppData\Local\DoubaoWork\User Data\sandbox_runtime\bases\fa694f517e5995233c80a9a8db5212c2\python\python.exe"

# 分区表（与 partitions_dual.csv 严格一致）
$SLOTS = @{
    bootloader      = 0x0
    partition_table = 0x8000
    ota_data        = 0xf000
    app_sel         = 0x20000     # 3 MB  选择器（factory）
    app_nav         = 0x320000    # 5 MB  导航（ota_0）
    app_navbk       = 0x820000    # 5 MB  导航备份（ota_1，本项目暂不烧）
    app_f1          = 0xD20000    # 5 MB  F1 看板（ota_2）
}

$FILES = @{
    bootloader      = @{ off = $SLOTS.bootloader;      path = "bootloader.bin" }
    partition_table = @{ off = $SLOTS.partition_table; path = "partition-table.bin" }
    ota_data        = @{ off = $SLOTS.ota_data;        path = "ota_data_initial.bin" }
    app_sel         = @{ off = $SLOTS.app_sel;         path = "app_sel\EVA_Selector.bin" }
    app_nav         = @{ off = $SLOTS.app_nav;         path = "app_nav\EVA_Nav.bin" }
    app_f1          = @{ off = $SLOTS.app_f1;          path = "app_f1\lvgl_demo_v9.bin" }
}

if (-not (Test-Path -LiteralPath $Py)) { throw "找不到 python：$Py" }

# 按模式挑选要写的槽
switch ($Only) {
    "sel" { $list = @("app_sel") }
    "nav" { $list = @("app_nav") }
    "f1"  { $list = @("app_f1") }
    default {
        $list = @("bootloader", "partition_table", "ota_data", "app_sel", "app_nav")
        if (-not $SkipF1) { $list += "app_f1" }
    }
}

$cmd = @("-m", "esptool", "--chip", "esp32s3", "--port", $Port, "-b", "$Baud",
         "--before", "default-reset", "--after", "hard-reset",
         "write-flash", "--flash-mode", "dio", "--flash-size", "32MB", "--flash-freq", "80m")

Write-Host "== EVA 双程序烧录 ($Only) 端口 $Port =="
foreach ($k in $list) {
    $f = $FILES[$k]
    $full = Join-Path $Root $f.path
    if (-not (Test-Path -LiteralPath $full)) { throw "缺文件：$full" }
    $size = (Get-Item -LiteralPath $full).Length
    $md5  = (Get-FileHash -LiteralPath $full -Algorithm MD5).Hash.ToLower()
    Write-Host ("  {0,-16} 0x{1:X6}  {2,9} B  {3}" -f $k, $f.off, $size, $md5)
    $cmd += @(("0x{0:X}" -f $f.off), $full)
}

Write-Host "`n开始写入……（带电池的板子拔 USB 不会断，烧录期间不要拔线）"
& $Py @cmd

if ($LASTEXITCODE -ne 0) { throw "esptool 返回 $LASTEXITCODE，烧录失败" }

Write-Host "`n烧录完成。上电后应先看到选择界面：左半屏（EVA 紫 / NERV 标志）进导航，右半屏（法拉利红 / F1 标志）进看板。"
Write-Host "若开机直接进了某个程序，说明该程序没把引导指针写回选择器，检查它的 hand_boot_back_to_selector() 日志。"
