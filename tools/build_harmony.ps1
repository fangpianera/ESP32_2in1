<#
  build_harmony.ps1 —— 鸿蒙端一键构建（带凭据自检 + 产物校验）

  用法：
    powershell -ExecutionPolicy Bypass -File tools\build_harmony.ps1
    powershell -ExecutionPolicy Bypass -File tools\build_harmony.ps1 -SkipVerify

  作用：
    1) 检查 Config.ets 里的 AMAP_WEB_KEY 不是 PASTE_ 占位、长度 32 —— 防止把"离线折线版"打进包里；
    2) 准备 hvigor 依赖联接与工具链环境变量；
    3) 跑 assembleHap；
    4) 解开包在 modules.abc 里反查 Key，确认真的编进去了。

  签名说明：
    命令行只有在工程里存在签名配置时才会产出 entry-default-signed.hap。
    本机 .sign/ 目录已丢失（证书 p12/cer/p7b 仍在 C:\Users\Administrator\.ohos\config\），
    需在 DevEco Studio 里「Project Structure → Signing Configs → 自动生成签名」重新生成一次，
    之后本脚本也会自动签出可安装的包。
#>
param(
  [switch]$SkipVerify
)

$ErrorActionPreference = 'Stop'

$proj = (Resolve-Path (Join-Path $PSScriptRoot '..\mobile\harmony')).Path
$cfg  = Join-Path $proj 'entry\src\main\ets\common\Config.ets'

# ---------- 1. 凭据自检 ----------
if (-not (Test-Path $cfg)) { throw "找不到 $cfg" }
$m = Select-String -Path $cfg -Pattern "AMAP_WEB_KEY: string = '([^']*)'"
if (-not $m) { throw "Config.ets 里没有 AMAP_WEB_KEY 定义" }
$key = $m.Matches[0].Groups[1].Value
if ([string]::IsNullOrWhiteSpace($key) -or $key.StartsWith('PASTE_')) {
  Write-Host "[X] AMAP_WEB_KEY 还是占位值「$key」——这样打出来的 App 只能画离线折线。" -ForegroundColor Red
  throw "请先把真实的高德 Web 服务 Key 填进 Config.ets 再构建"
}
Write-Host ("[OK] 高德 Key 已填：" + $key.Substring(0, [Math]::Min(8, $key.Length)) + "…（" + $key.Length + " 位）") -ForegroundColor Green
if ($key.Length -ne 32) { Write-Host "[!] Key 长度不是 32 位，确认没有粘错或漏字符" -ForegroundColor Yellow }

# 固件侧凭据（顺手提醒，不影响 App 构建）
$secrets = Join-Path $PSScriptRoot '..\firmware\app_f1\main\secrets.h'
if (Test-Path $secrets) {
  Write-Host "[OK] firmware/app_f1/main/secrets.h 存在（重新编 F1 固件会带上真实 Wi-Fi）" -ForegroundColor Green
} else {
  Write-Host "[!] 没有 firmware/app_f1/main/secrets.h —— 重编 F1 固件会得到连不上网的版本" -ForegroundColor Yellow
}

# ---------- 2. 工具链 ----------
$deveco = 'D:\Huawei\DevEcoStudio'
$hvigor = Join-Path $deveco 'tools\hvigor\hvigor\bin\hvigor.js'
$node   = Join-Path $deveco 'tools\node\node.exe'
foreach ($p in @($hvigor, $node)) { if (-not (Test-Path $p)) { throw "工具链缺失：$p" } }

$jm = Join-Path $proj 'node_modules\@ohos'
if (-not (Test-Path $jm)) { New-Item -ItemType Directory -Force $jm | Out-Null }
$jp = Join-Path $jm 'hvigor-ohos-plugin'
if (-not (Test-Path $jp)) {
  & "$env:SystemRoot\System32\cmd.exe" /c mklink /J $jp (Join-Path $deveco 'tools\hvigor\hvigor-ohos-plugin') | Out-Null
}

$env:DEVECO_SDK_HOME = Join-Path $deveco 'sdk'
$env:NODE_PATH        = Join-Path $deveco 'tools\hvigor\node_modules'
$env:JAVA_HOME        = Join-Path $deveco 'jbr'
$env:PATH             = "$(Join-Path $deveco 'jbr\bin');$(Join-Path $deveco 'tools\node');$(Join-Path $deveco 'tools\ohpm\bin');$env:PATH"

# ---------- 3. 构建 ----------
Push-Location $proj
try {
  & $node $hvigor --no-daemon --mode module -p product=default -p module=entry@default assembleHap
  if ($LASTEXITCODE -ne 0) { throw "hvigor 构建失败（exit $LASTEXITCODE）" }
} finally {
  Pop-Location
}

$outDir = Join-Path $proj 'entry\build\default\outputs\default'
$haps = Get-ChildItem $outDir -Filter '*.hap' -ErrorAction SilentlyContinue
if (-not $haps) { throw "构建完成但没找到 hap：$outDir" }
foreach ($h in $haps) { Write-Host ("[产物] " + $h.FullName + "  " + $h.Length + " B") -ForegroundColor Cyan }
if (-not ($haps | Where-Object { $_.Name -like '*signed*' })) {
  Write-Host "[!] 只有未签名包，装不上机。到 DevEco 里做一次自动签名（Project Structure → Signing Configs）后重跑本脚本。" -ForegroundColor Yellow
}

# ---------- 4. 产物校验：Key 是否真的进包 ----------
if (-not $SkipVerify) {
  $hap = ($haps | Where-Object { $_.Name -like '*signed*' } | Select-Object -First 1)
  if (-not $hap) { $hap = $haps[0] }
  $tmp = Join-Path $env:TEMP ('hapx_' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
  New-Item -ItemType Directory -Force $tmp | Out-Null
  try {
    Expand-Archive -LiteralPath $hap.FullName -DestinationPath $tmp -Force
    $hit = $false
    Get-ChildItem $tmp -Recurse -File | ForEach-Object {
      $bytes = [System.IO.File]::ReadAllBytes($_.FullName)
      if ([System.Text.Encoding]::ASCII.GetString($bytes).Contains($key)) {
        $hit = $true
        Write-Host ("[OK] 包内命中该 Key：" + $_.Name) -ForegroundColor Green
      }
    }
    if (-not $hit) { Write-Host "[X] 包内没搜到该 Key，构建可能没吃这份源码" -ForegroundColor Red }
  } finally {
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
  }
}
