# 把 D:\EVA_F1_ESP32 推到 GitHub
#
# 前提：安装 Git for Windows（https://git-scm.com/download/win），装完重开终端让 PATH 生效。
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

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Host "未找到 git，请先安装 Git for Windows 并重开终端。" -ForegroundColor Red
    exit 1
}

Set-Location $Repo

if (-not (Test-Path ".git")) {
    git init
    git config core.autocrlf false
    git config core.longpaths true
}

$userName = git config user.name 2>$null
if ([string]::IsNullOrWhiteSpace($userName)) { git config user.name "EVA-F1 Archive" }
$userMail = git config user.email 2>$null
if ([string]::IsNullOrWhiteSpace($userMail)) { git config user.email "archive@local" }

git checkout -b $Branch 2>$null | Out-Null

git add -A
git commit -m "archive: EVA x F1 ESP32-S3 triple-slot firmware, frozen 2026-11-20"

if ($Remote -ne "") {
    $existing = git remote get-url origin 2>$null
    if ($existing) { git remote remove origin }
    git remote add origin $Remote
    git branch -M $Branch
    git push -u origin $Branch
    Write-Host "已推送到 $Remote ($Branch)" -ForegroundColor Green
} else {
    Write-Host "本地提交完成。推送时执行：" -ForegroundColor Yellow
    Write-Host "  git remote add origin <你的仓库地址>"
    Write-Host "  git push -u origin $Branch"
}
