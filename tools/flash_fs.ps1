<#
.SYNOPSIS
  把 storage/ 目录打包成 LittleFS 镜像并烧到开发板 storage 分区
.DESCRIPTION
  前提：
    1. 已执行 tools\build_fonts.ps1 与 tools\build_icons.ps1，storage/ 下有 fonts 与 icon
    2. ESP-IDF 环境已安装（或在 ESP-IDF PowerShell 里运行，$env:IDF_PATH 存在）
    3. 板子插在 COM5（可用 -Port 改）

  分区参数必须与 partitions_eva.csv 一致：
    storage 起始 0xA20000，大小 0x1400000（20MB，32MB Flash 版）
    若用 16MB 备选表：起始 0x620000，大小 0x9C0000
#>
param(
    [string]$Port        = "COM5",
    [string]$StorageDir  = "$PSScriptRoot\..\storage",
    [string]$Offset      = "0xA20000",
    [string]$Size        = "0x1400000",
    [string]$OutBin      = "$PSScriptRoot\..\build\storage.bin",
    [switch]$OnlyBuild
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path $StorageDir)) { throw "资源目录不存在：$StorageDir（先跑 build_fonts / build_icons）" }

# ---- mklittlefs 定位：ESP-IDF 自带，或 PATH 里的 ----
$mklfs = $null
$cands = @(
    (Join-Path $env:IDF_PATH "components\littlefs\mklittlefs\mklittlefs.exe"),
    (Join-Path $env:IDF_PATH "tools\mklittlefs\mklittlefs.exe")
)
foreach ($c in $cands) { if ($c -and (Test-Path $c)) { $mklfs = $c; break } }
if (-not $mklfs) {
    $cmd = Get-Command mklittlefs -ErrorAction SilentlyContinue
    if ($cmd) { $mklfs = $cmd.Source }
}
if (-not $mklfs) {
    throw "找不到 mklittlefs。二选一：① 在 ESP-IDF 命令行里运行本脚本；② pip install littlefs-python 后用 tools\pack_lfs.py"
}

New-Item -ItemType Directory -Force -Path (Split-Path $OutBin) | Out-Null
Remove-Item $OutBin -ErrorAction SilentlyContinue

# block size 4096 / page 256 / 无压缩 —— 必须与固件 CONFIG_LITTLEFS_* 一致
& $mklfs -c $StorageDir -b 4096 -p 256 -s $Size -d 5 $OutBin
if ($LASTEXITCODE -ne 0) { throw "mklittlefs 失败（exit=$LASTEXITCODE）" }
Write-Host ("镜像生成：{0}  {1:N1} KB" -f $OutBin, ((Get-Item $OutBin).Length/1KB)) -ForegroundColor Green

if ($OnlyBuild) { return }

# ---- 烧录 ----
$esptool = "python"
$args = @("-m", "esptool", "--port", $Port, "--baud", "921600",
          "write_flash", "--flash_mode", "dio", "--flash_size", "detect",
          $Offset, $OutBin)
Write-Host "[flash] $esptool $($args -join ' ')" -ForegroundColor Cyan
& $esptool @args
if ($LASTEXITCODE -ne 0) { throw "烧录失败（检查 COM 口是否被占用、板子是否在下载模式）" }

Write-Host @"

烧录完成。验证方式：
  1) 串口日志应出现：  res: littlefs 12345/20480 KB used
  2) 若日志出现 "littlefs mount failed"，先确认 partitions_eva.csv 的 storage 偏移没写错
  3) 只更新资源、不重烧固件时，用本脚本单独跑即可（不要整片 erase_flash，会清掉路线缓存）

"@ -ForegroundColor Yellow
