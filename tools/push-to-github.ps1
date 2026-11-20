# 把 D:\EVA_F1_ESP32 推到 GitHub
#
# Git 来源（二选一，脚本会自动找）：
#   1) PortableGit（本机已有）：C:\Users\Administrator\.workbuddy\binaries\PortableGit\versions\1.3.0\cmd\git.exe
#   2) 正式安装的 Git for Windows（https://git-scm.com/download/win，装完重开终端）
#
# 用法：
#   .\push-to-github.ps1                                              # 只做本地 init + 首次提交
#   .\push-to-github.ps1 -Remote https://github.com/<你>/<仓库>.git     # 顺带推送
#
# 说明：仓库已带 .gitignore，build/、managed_components/、sdkconfig、日志都不进历史；
#      release/ 下的 .bin 例外放行，作为归档产物随仓库保存。

param(
    [string]$Remote = "",
    [string]$Branch = "main"
)

$Repo = "D:\EVA_F1_ESP32"

$GitCandidates = @(
    "C:\Users\Administrator\.workbuddy\binaries\PortableGit\versions\1.3.0\cmd\git.exe",
    "C:\Program Files\Git\cmd\git.exe",
    "C:\Program Files (x86)\Git\cmd\git.exe"
)

$Git = $null
foreach ($c in $GitCandidates) { if (Test-Path $c) { $Git = $c; break } }
if (-not $Git) {
    $cmd = Get-Command git -ErrorAction SilentlyContinue
    if ($cmd) { $Git = $cmd.Source }
}
if (-not $Git) {
    Write-Host "未找到 git。请安装 Git for Windows 并重开终端。" -ForegroundColor Red
    exit 1
}
Write-Host "使用 git: $Git" -ForegroundColor Cyan

Set-Location $Repo

if (-not (Test-Path ".git")) {
    & $Git init | Out-Null
    & $Git config core.autocrlf false
    & $Git config core.longpaths true
}

$userName = (& $Git config user.name) 2>$null
if ([string]::IsNullOrWhiteSpace($userName)) { & $Git config user.name "EVA-F1 Archive" }
$userMail = (& $Git config user.email) 2>$null
if ([string]::IsNullOrWhiteSpace($userMail)) { & $Git config user.email "archive@local" }

& $Git checkout -b $Branch 2>$null | Out-Null

& $Git add -A
& $Git commit -m "archive: EVA x F1 ESP32-S3 triple-slot firmware, frozen 2026-11-20"

if ($Remote -ne "") {
    $existing = (& $Git remote get-url origin) 2>$null
    if ($existing) { & $Git remote remove origin }
    & $Git remote add origin $Remote
    & $Git branch -M $Branch
    & $Git push -u origin $Branch
    Write-Host "已推送到 $Remote ($Branch)" -ForegroundColor Green
} else {
    Write-Host "本地提交完成。推送时执行：" -ForegroundColor Yellow
    Write-Host "  $Git remote add origin <你的仓库地址>"
    Write-Host "  $Git push -u origin $Branch"
}
