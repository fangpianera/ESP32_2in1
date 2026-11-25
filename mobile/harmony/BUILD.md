# 鸿蒙端构建备忘（2026-11-18 实测）

> 本文每个数字都来自本机真实执行，不是推演。

## 1. 工具链（已装好）

| 项 | 路径 / 版本 |
|---|---|
| DevEco Studio | `D:\Huawei\DevEcoStudio`（26.0.1.290，安装 11.3 GB，C 盘只剩 4.8 GB 所以全进 D 盘） |
| SDK | `D:\Huawei\DevEcoStudio\sdk\default`，**API 26 / platformVersion 26.0.1 / Beta1** |
| node | `D:\Huawei\DevEcoStudio\tools\node\node.exe`（v24.14.1） |
| hvigor | `D:\Huawei\DevEcoStudio\tools\hvigor\hvigor`（6.26.20） |
| hvigor 插件 | `D:\Huawei\DevEcoStudio\tools\hvigor\hvigor-ohos-plugin`（6.26.20） |
| ohpm | `D:\Huawei\DevEcoStudio\tools\ohpm` |
| hdc | `D:\Huawei\DevEcoStudio\sdk\default\openharmony\toolchains\hdc.exe`（3.2.0f） |
| java | `D:\Huawei\DevEcoStudio\jbr\bin\java.exe`（打包 HAP 必需，缺了报 `spawn java ENOENT`） |

## 2. 两条硬约束（踩过的坑）

1. **工程路径不能含中文**：hvigor 直接报 `00306003 Invalid project path`。
   所以真源留在 `D:\豆包ESP32项目\EVA_Nav\harmony`（按你的要求），
   **构建在 ASCII 副本 `D:\EVA_Nav_harmony`** 进行，两边用 robocopy 同步。
2. **`@ohos/hvigor-ohos-plugin` 必须能被解析**：命令行构建需在工程里做目录联接，
   并给 hvigor 补 NODE_PATH（见第 3 节脚本）。

## 3. 命令行构建（等价于 DevEco 里的 Build）

```powershell
$src="D:\豆包ESP32项目\EVA_Nav\harmony"; $dst="D:\EVA_Nav_harmony"
robocopy $src $dst /E /XD node_modules .hvigor build oh_modules

# 一次性准备（已做过，重装 DevEco 后要重做）
New-Item -ItemType Directory -Force "$dst\node_modules\@ohos"
cmd /c mklink /J "$dst\node_modules\@ohos\hvigor-ohos-plugin" "D:\Huawei\DevEcoStudio\tools\hvigor\hvigor-ohos-plugin"

$env:DEVECO_SDK_HOME="D:\Huawei\DevEcoStudio\sdk"
$env:NODE_PATH="D:\Huawei\DevEcoStudio\tools\hvigor\node_modules"
$env:JAVA_HOME="D:\Huawei\DevEcoStudio\jbr"
$env:PATH="D:\Huawei\DevEcoStudio\jbr\bin;D:\Huawei\DevEcoStudio\tools\node;D:\Huawei\DevEcoStudio\tools\ohpm\bin;$env:PATH"
cd $dst
& "D:\Huawei\DevEcoStudio\tools\node\node.exe" `
  "D:\Huawei\DevEcoStudio\tools\hvigor\hvigor\bin\hvigor.js" `
  --no-daemon --mode module -p product=default -p module=entry@default assembleHap
```

产物：`D:\EVA_Nav_harmony\entry\build\default\outputs\default\entry-default-unsigned.hap`
**实测结果：BUILD SUCCESSFUL，150,440 B（未签名，不能装机）。**

## 4. 按真实 SDK 修掉的 4 类错误（旧代码是照文档猜的，编译不过）

| 旧写法 | 真接口（API 26 实测） |
|---|---|
| `socket.TCPSocketMessageInfo` | `socket.SocketMessageInfo` |
| `geoLocationManager.LocationScenario.SCENE_NAVIGATION`（枚举只剩 `UNSET`） | `ContinuousLocationRequest{ interval, locationScenario: UserActivityScenario.NAVIGATION }` |
| `healthStore.dataType.*` / `requestAuthorization` / `getStepData` | `healthStore.init(ctx)` → `requestAuthorizations(ctx, {readDataTypes:[{id}], writeDataTypes:[]})` → `readData<SamplePoint>({startTime,endTime,samplePointDataType:{id}})` |
| `as unknown as Record<string,Object>` | ArkTS 禁止 `any/unknown`，改为显式字段拷贝 |

另：`module.json5` 的 `backgroundModes` 在 API 26 已**下沉到 abilities[] 里**，写在 module 级会 schema 校验失败。

## 4.5 签名（2026-11-19 实测打通）

DevEco 26 的「自动签名」**不写 build-profile.json5**，而是写到工程根的
`.sign/sign-config.json5`（`signingConfigs` + 加密后的 `storePassword`/`keyPassword`），
证书材料在 `C:\Users\<你>\.ohos\config\default_<工程名>_<hash>.{p12,cer,csr,p7b}`。

命令行要签出包，必须两处齐备：
1. `build-profile.json5` 的 `app.products[0]` 加 `"signingConfig": "default"`；
2. `.sign/sign-config.json5` 存在（同名时它优先，hvigor 会打 WARN 说明这点）。

```powershell
# 一次性：把 .sign 的配置内联进 build-profile（本机已做，真源里保留 signingConfig 绑定即可）
```
签名产物：`entry-default-signed.hap`（190,685 B）→ `hdc install -r` 成功；
设备上包名 `com.evanav.harmony`，桌面名 `EVA Nav`。

> 2026-11-25 现状：`.sign/` 已随旧构建副本一起清掉，证书材料（p12/cer/csr/p7b）仍在
> `C:\Users\Administrator\.ohos\config\`，命令行只会产出 unsigned 包。
> 在 DevEco 里重做一次「Project Structure → Signing Configs → 自动生成签名」即可恢复，
> 之后 `tools\build_harmony.ps1` 能直接签出可装机包。

装机与调试命令（实测可用）：

```powershell
$hdc="D:\Huawei\DevEcoStudio\sdk\default\openharmony\toolchains\hdc.exe"
& $hdc install -r D:\EVA_Nav_harmony\entry\build\default\outputs\default\entry-default-signed.hap
& $hdc shell aa force-stop com.evanav.harmony
& $hdc shell aa start -a EntryAbility -b com.evanav.harmony     # 启动
& $hdc shell "snapshot_display -f /data/local/tmp/a.jpeg"        # 截图
& $hdc file recv /data/local/tmp/a.jpeg D:\out\a.jpeg
& $hdc shell hilog -x | Select-String "\[EVA\]"                 # 只看自己的日志
& $hdc shell "uinput -T -c 185 380"                             # 按下「启动链路」
```

## 5. 还没做的（需要你出手）

1. **签名**：`entry-default-unsigned.hap` 装不上。要么在 DevEco 登录华为账号用「自动签名」，
   要么在 AGC 建应用（包名 `com.evanav.harmony`）→ 申请调试证书 + Profile 手动配。
2. **手机**：打开开发者模式 + USB 调试，插上后 `hdc list targets` 应能看到设备；
   然后我要读 `const.ohos.apiversion` 确认这台「鸿蒙7」的 API 级别，
   决定 `compatibleSdkVersion` 从现在的 `5.0.0(12)` 是否要提高。
3. **高德 Web 服务 Key**：已填入 `entry/src/main/ets/common/Config.ets` 的 `AMAP_WEB_KEY`
   （本机真值，文件带 `assume-unchanged`，不进仓库）。构建前跑 `tools\build_harmony.ps1`
   会自动自检这一步：占位值直接拦下，构建后再解包反查 Key 是否真进了 `modules.abc`。
4. **受限权限（ACL）**：`LOCATION_IN_BACKGROUND`、`READ_HEALTH_DATA` 已在 module.json5 里注释掉，
   拿到 ACL 后再放开；不放开时锁屏后定位会被冻结，30 s 校准会断。
5. **健康数据类型 ID**：SDK 只给了 `DataType{id:number}`，没给常量；
   `Config.ets` 里三个 ID 默认 0 = 不启用健康数据，只走身高经验式。
