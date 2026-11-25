# =============================================================================
# flash_all.ps1 — 三程序一键烧录（选择器 + EVA 导航 + F1 看板 + MotoNav）
#
# 用法：
#   .\flash_all.ps1                 # 全量：bootloader + 分区表 + otadata + 四个 app
#   .\flash_all.ps1 -Port COM7      # 指定端口
#   .\flash_all.ps1 -SkipF1         # 跳过 F1（槽留空）
#   .\flash_all.ps1 -SkipMoto       # 跳过 MotoNav
#   .\flash_all.ps1 -Only nav       # 只重烧导航这一片（日常改导航用）
#   .\flash_all.ps1 -Only reset     # 应急：只清引导记录，让板子下次启动回选择器
#
# 关键约定：
#   * 必须用本目录的分区表：槽偏移见 $SLOTS，改一处就整体错位。
#   * -Only 模式不要带 bootloader/分区表，否则可能把别的槽写坏。
#   * 不要用 `idf.py flash`：它默认把 app 烧到 0x20000（现在是 app_sel，2 MB），
#     导航镜像 3.5 MB 塞不进去，会溢出写坏 app_nav。
#   * 分区表把 otadata 放回了 0xF000，nvs 在 0x9000–0xF000（旧 MotoNav 的 otadata
#     正好压在 0xE000–0xF000 这段 nvs 里）；all 模式先擦 0x9000 起 0x8000，
#     一次清干净 nvs + otadata，避免旧引导记录和旧 nvs 键值残留。
#   * MotoNav 槽按"原 3 MB 整槽"写入（motonav_slot3m.bin），不是只写 1.5 MB 镜像，
#     这样槽尾残留也和备份一致，回滚最省事。
#   * bootloader 已开 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE：选择器写槽时记录 state=NEW，
#     程序启动后变 PENDING_VERIFY，谁都没 mark valid —— 下次复位 bootloader 就把它标
#     ABORTED 并回退 factory（选择器）。MotoNav 不会自己回跳，靠这条兜底"只跑一个上电周期"。
# =============================================================================

param(
    [string]$Port = "COM5",
    [ValidateSet("all", "sel", "nav", "f1", "moto", "reset")][string]$Only = "all",
    [switch]$SkipF1,
    [switch]$SkipMoto,
    [int]$Baud = 460800
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path

# 优先用 IDF 工具链自带的 python（旧的 sandbox_runtime 路径已失效）
$Candidates = @(
    "D:\Espressif\tools\python\v5.5.6\venv\Scripts\python.exe",
    "C:\Users\Administrator\AppData\Local\DoubaoWork\User Data\sandbox_runtime\bases\fa694f517e5995233c80a9a8db5212c2\python\python.exe"
)
$Py = $Candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $Py) { throw "找不到 python，检查 D:\Espressif 工具链" }

# 分区表（与 partitions_dual.csv 严格一致）
$SLOTS = @{
    bootloader      = 0x0
    partition_table = 0x8000
    ota_data        = 0xf000
    app_sel         = 0x20000     # 2 MB  选择器（factory）
    app_nav         = 0x220000    # 5 MB  导航（ota_0）
    app_f1          = 0x720000    # 2 MB  F1 看板（ota_1）
    app_moto        = 0x920000    # 3 MB  MotoNav（ota_2）
}

$FILES = @{
    bootloader      = @{ off = $SLOTS.bootloader;      path = "bootloader.bin" }
    partition_table = @{ off = $SLOTS.partition_table; path = "partition-table.bin" }
    ota_data        = @{ off = $SLOTS.ota_data;        path = "ota_data_initial.bin" }
    app_sel         = @{ off = $SLOTS.app_sel;         path = "app_sel\EVA_Selector.bin" }
    app_nav         = @{ off = $SLOTS.app_nav;         path = "app_nav\EVA_Nav.bin" }
    app_f1          = @{ off = $SLOTS.app_f1;          path = "app_f1\lvgl_demo_v9.bin" }
    app_moto        = @{ off = $SLOTS.app_moto;        path = "app_moto\motonav_slot3m.bin" }
}

# 按模式挑选要写的槽
switch ($Only) {
    "sel"  { $list = @("app_sel") }
    "nav"  { $list = @("app_nav") }
    "f1"   { $list = @("app_f1") }
    "moto" { $list = @("app_moto") }
    default {
        $list = @("bootloader", "partition_table", "ota_data", "app_sel", "app_nav")
        if (-not $SkipF1)   { $list += "app_f1" }
        if (-not $SkipMoto) { $list += "app_moto" }
    }
}

function Invoke-Esptool {
    param([string[]]$MoreArgs)
    $cmd = @("-m", "esptool", "--chip", "esp32s3", "--port", $Port, "-b", "$Baud") + $MoreArgs
    & $Py @cmd
    if ($LASTEXITCODE -ne 0) { throw "esptool 返回 $LASTEXITCODE，烧录失败" }
}

Write-Host "== 三程序烧录 ($Only) 端口 $Port =="

# 应急模式：只清引导记录。MotoNav 之类不回跳的程序，重启后靠这条命令回选择器
if ($Only -eq "reset") {
    Write-Host "  清空 otadata（0xF000–0x11000）..."
    Invoke-Esptool -MoreArgs @("--before", "default_reset", "--after", "hard_reset",
                               "erase_region", "0xF000", "0x2000")
    Write-Host "`n已清空引导记录，板子重启后进入选择器界面。"
    exit 0
}

# all 模式先擦 nvs + otadata 区（0x9000–0x11000），清掉 MotoNav 时代的旧引导记录与 nvs 键值
if ($Only -eq "all") {
    Write-Host "  擦除 nvs + otadata 区 0x9000 0x8000 ..."
    Invoke-Esptool -MoreArgs @("--before", "default_reset", "--after", "no_reset",
                               "erase_region", "0x9000", "0x8000")
}

$cmdArgs = @("--before", "default_reset", "--after", "hard_reset",
             "write_flash", "--flash_mode", "dio", "--flash_size", "32MB", "--flash_freq", "80m")

foreach ($k in $list) {
    $f = $FILES[$k]
    $full = Join-Path $Root $f.path
    if (-not (Test-Path -LiteralPath $full)) { throw "缺文件：$full" }
    $size = (Get-Item -LiteralPath $full).Length
    $md5 = (Get-FileHash -LiteralPath $full -Algorithm MD5).Hash.ToLower()
    Write-Host ("  {0,-16} 0x{1:X6}  {2,9} B  {3}" -f $k, $f.off, $size, $md5)
    $cmdArgs += @(("0x{0:X}" -f $f.off), $full)
}

Write-Host "`n开始写入……（烧录期间不要拔线）"
Invoke-Esptool -MoreArgs $cmdArgs

Write-Host "`n烧录完成。上电后先看到选择界面："
Write-Host "  左上（EVA 紫 / NERV 徽标） → EVA 导航"
Write-Host "  右上（法拉利红 / F1 logo）→ F1 看板"
Write-Host "  下半（科技蓝 / 摩托车）    → MotoNav"
Write-Host "任何程序退出后重启都应回到这里（选择器启动时自己把引导指针收回 factory）。"
Write-Host "若开机直接进了某个程序，说明分区表没烧对或 otadata 残留没擦净。"

