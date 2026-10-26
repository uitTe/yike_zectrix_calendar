# AGC OAuth 与中转云函数接口调用说明

编写日期：2026年10月22日。项目目录：`D:\zectrix\zectrix_yike`。

请求配置来自当前 `tools/agc_probe.py`；响应记录来自北京时间 2026年10月22日 17:40 的实际调用。文档中的项目参数（Client ID、productId、projectId、groupId）、业务密钥、令牌和 Cookie 全部以尖括号占位符表示，真实值只保存在 `tools/agc_probe.py` 与设备 NVS 中。

## 1. 调用顺序

1. 向 OAuth 接口发送 Client ID、Client Secret 和 `useJwt: 0`。
2. 从 JSON 响应中取得 `access_token`。
3. 将该令牌放入云函数请求头 `Authorization: Bearer <access_token>`。
4. 向中转接口发送 `apiKey`、`data.groupId` 和 `data.startDate`。
5. 检查 HTTP 状态、业务响应和日期数据。

## 2. 当前配置

| 配置项 | 当前内容 |
| --- | --- |
| Python 解释器 | `D:\zectrix\zectrix_yike\tools\.venv\Scripts\python.exe` |
| 调用脚本 | `D:\zectrix\zectrix_yike\tools\agc_probe.py` |
| Client ID | `<client-id>` |
| Client Secret | `<client-secret>` |
| 请求头 productId | `<product-id>` |
| 云函数业务密钥 | 环境变量名 `CUSTOM_API_KEY`，值 `<api-key>` |
| OAuth 参数 | `useJwt: 0`，注意字段大小写 |
| 生效的云函数配置 | `resolve_config()` 中的 `"PROXY_URL": PROXY_URL_ORI` |
| 请求超时 | `timeout=15`，单位为秒 |
| 证书验证 | 使用 `requests` 默认设置，启用验证 |
| 日志响应体长度上限 | `BODY_LIMIT=8000`，单位为字符 |
| 分组编号 | `GROUP_ID = "<group-id>"` |
| 查询开始日期 | `START_DATE` 留空时使用北京时间当天日期 |

当前生效的 OAuth 地址：

```text
https://connect-drcn.dbankcloud.cn/agc/apigw/oauth2/v1/token
```

当前生效的云函数地址，取自 `PROXY_URL_ORI`：

```text
https://connect-drcn.dbankcloud.cn/agc/apigw/wisefunction/functions/<project-id>/proxy-function-$latest
```

## 3. 运行项目脚本

在 PowerShell 中检查参数是否齐备：

```powershell
Set-Location 'D:\zectrix\zectrix_yike'
& '.\tools\.venv\Scripts\python.exe' -B '.\tools\agc_probe.py' --check
```

`--check` 不发送网络请求，不验证凭据有效性。

执行完整调用：

```powershell
Set-Location 'D:\zectrix\zectrix_yike'
$env:PYTHONIOENCODING = 'utf-8'
& '.\tools\.venv\Scripts\python.exe' -B '.\tools\agc_probe.py'
$LASTEXITCODE
```

当前脚本默认读取文件顶部的配置。退出码 `0` 表示脚本的调用与数据检查通过；退出码 `1` 表示脚本报告失败。任一步失败后终止后续处理。

脚本会打印原始响应体，其中可能包含完整令牌及 Cookie；对外提供日志时需要脱敏。

## 4. OAuth 请求与实测响应

### 4.1 请求地址及请求头

```http
POST https://connect-drcn.dbankcloud.cn/agc/apigw/oauth2/v1/token
Content-Type: application/json
```

### 4.2 完整请求体

```json
{
  "grant_type": "client_credentials",
  "client_id": "<client-id>",
  "client_secret": "<client-secret>",
  "useJwt": 0
}
```

| 字段 | 实际类型及用途 |
| --- | --- |
| `grant_type` | 字符串，当前固定为 `client_credentials` |
| `client_id` | 字符串，OAuth Client ID |
| `client_secret` | 字符串，与该 Client ID 配套的凭据 |
| `useJwt` | JSON 数字，当前发送 `0` |

脚本调用语句：

```python
resp = requests.post(cfg["TOKEN_URL"], json=payload, timeout=TIMEOUT)
```

### 4.3 实测响应头

请求耗时：112 毫秒。服务端时间：北京时间 `2026-10-22 17:40:55`。

```http
200 OK
Date: Thu, 22 Oct 2026 09:40:55 GMT
Content-Type: application/json; charset=utf-8
Content-Length: 256
Connection: keep-alive
Set-Cookie: <cookie>
x-error-code: 0x00000000, 0x00000000
x-nuwa-request-microservice: X
x-nuwa-sample-state: planned_no_sample
Access-Control-Allow-Origin: *
Access-Control-Max-Age: 86400
Access-Control-Allow-Methods: POST,PUT,GET,OPTIONS,DELETE
Access-Control-Allow-Headers: Accept,Content-Type,Referer,User-Agent,client_id,app_id,access_token,Authorization,algorithm_type,sdkVersion,sdkPlatform,productId,appId,tag,sdkServiceName,clientId,accessToken,sdkPlatformVersion,packageName,appVersion,aaId,sdkType,x-apik,x-cert-fp
content-encoding: gzip
X-XSS-Protection: 1; mode=block
X-frame-options: SAMEORIGIN
X-Content-Type-Options: nosniff
Server: elb
```

### 4.4 完整响应体

```json
{
  "access_token": "<access-token>",
  "expires_in": 175480
}
```

本次令牌长度为 227 个字符。`expires_in` 的单位为秒，具体数值使用每次实际响应。脚本要求响应为 HTTP 200、合法 JSON，且包含非空 `access_token`。

## 5. 云函数请求与实测响应

### 5.1 请求地址及请求头

```http
POST https://connect-drcn.dbankcloud.cn/agc/apigw/wisefunction/functions/<project-id>/proxy-function-$latest
clientId: <client-id>
Authorization: Bearer <本次 OAuth 返回的 access_token>
productId: <product-id>
Content-Type: application/json
```

`Authorization` 使用 `Bearer`、一个空格和完整令牌。当前脚本显式设置的请求头字段为 `productId`。

### 5.2 完整请求体

```json
{
  "apiKey": "<api-key>",
  "data": {
    "groupId": "<group-id>",
    "startDate": "2026-10-22"
  }
}
```

| 字段 | 实际类型及用途 |
| --- | --- |
| `apiKey` | 字符串，来自当前脚本的 `CUSTOM_API_KEY` |
| `data` | JSON 对象 |
| `data.groupId` | 字符串，本次为 `<group-id>` |
| `data.startDate` | 字符串，格式为 `YYYY-MM-DD`，本次为 `2026-10-22` |

脚本调用语句：

```python
resp = requests.post(cfg["PROXY_URL"], headers=headers, json=body, timeout=TIMEOUT)
```

### 5.3 实测响应头

请求耗时：1280 毫秒。服务端时间：北京时间 `2026-10-22 17:40:56`。

```http
200 OK
Date: Thu, 22 Oct 2026 09:40:56 GMT
Content-Type: application/json; charset=utf-8
Content-Length: 340
Connection: keep-alive
Set-Cookie: <cookie>
x-error-code: 0x000000c8
content-encoding: gzip
X-XSS-Protection: 1; mode=block, 1; mode=block
X-frame-options: SAMEORIGIN, SAMEORIGIN
X-Content-Type-Options: nosniff, nosniff
Access-Control-Allow-Origin: *
Access-Control-Max-Age: 86400
Access-Control-Allow-Methods: POST,PUT,GET,OPTIONS,DELETE
Access-Control-Allow-Headers: Accept,Content-Type,Referer,User-Agent,client_id,app_id,access_token,Authorization,algorithm_type,sdkVersion,sdkPlatform,productId,appId,tag,sdkServiceName,clientId,accessToken,sdkPlatformVersion,packageName,appVersion,aaId,sdkType,x-apik,x-cert-fp
Server: elb
```

### 5.4 响应体与执行结果

下面的班次名与分组编号是合成样例，结构与字段形态取自实测响应：真实排班数据不进仓库。同一份样例就是 `tools/testdata/agc_response_20261022.json`，板端自检与 PC 侧测试都用它。

```json
{
  "code": 200,
  "message": "查询成功",
  "data": {
    "groupId": "grp_example_0000000000000",
    "startDate": "2026-10-22",
    "endDate": "2026-11-04",
    "days": [
      {
        "date": "2026-10-22",
        "shiftName": "休",
        "amStart": "00:00",
        "amEnd": "00:00",
        "pmStart": "00:00",
        "pmEnd": "00:00",
        "remark": ""
      },
      {
        "date": "2026-10-23",
        "shiftName": "A1",
        "amStart": "12:30",
        "amEnd": "17:00",
        "pmStart": "18:00",
        "pmEnd": "21:00",
        "remark": ""
      },
      {
        "date": "2026-10-24",
        "shiftName": "C2（验车）",
        "amStart": "09:00",
        "amEnd": "12:00",
        "pmStart": "13:15",
        "pmEnd": "18:00",
        "remark": ""
      },
      {
        "date": "2026-10-25",
        "shiftName": "休",
        "amStart": "00:00",
        "amEnd": "00:00",
        "pmStart": "00:00",
        "pmEnd": "00:00",
        "remark": ""
      },
      {
        "date": "2026-10-26",
        "shiftName": "B1",
        "amStart": "09:30",
        "amEnd": "13:15",
        "pmStart": "17:00",
        "pmEnd": "21:00",
        "remark": ""
      },
      {
        "date": "2026-10-27",
        "shiftName": "休",
        "amStart": "00:00",
        "amEnd": "00:00",
        "pmStart": "00:00",
        "pmEnd": "00:00",
        "remark": ""
      },
      {
        "date": "2026-10-28",
        "shiftName": "A2",
        "amStart": "09:00",
        "amEnd": "13:00",
        "pmStart": "14:15",
        "pmEnd": "18:00",
        "remark": ""
      },
      {
        "date": "2026-10-29",
        "shiftName": "A2",
        "amStart": "09:00",
        "amEnd": "13:00",
        "pmStart": "14:15",
        "pmEnd": "18:00",
        "remark": ""
      },
      {
        "date": "2026-10-30",
        "shiftName": "A2",
        "amStart": "09:00",
        "amEnd": "13:00",
        "pmStart": "14:15",
        "pmEnd": "18:00",
        "remark": ""
      },
      {
        "date": "2026-10-31",
        "shiftName": "休",
        "amStart": "00:00",
        "amEnd": "00:00",
        "pmStart": "00:00",
        "pmEnd": "00:00",
        "remark": ""
      },
      {
        "date": "2026-11-01",
        "remark": ""
      },
      {
        "date": "2026-11-02",
        "remark": ""
      },
      {
        "date": "2026-11-03",
        "remark": ""
      },
      {
        "date": "2026-11-04",
        "remark": ""
      }
    ]
  }
}
```

本次业务码为 `200`，消息为“查询成功”。脚本完成日期和时间字段检查，退出码为 `0`。返回 `data.days` 共 14 条，日期范围为 `2026-10-22` 至 `2026-11-04`。11月1日至4日的条目只有 `date` 与空 `remark`，`shiftName`、`amStart`、`amEnd`、`pmStart`、`pmEnd` 全部缺省。其余条目六个字段齐全。

### 5.5 字段缺省的语义

**字段缺省表示该日期没有排班数据。** 服务端不发送空字符串，也不发送 `null`，直接把键省略。这是正常的业务结果，不是错误，也不是响应被截断。

因此判据分两层：

| 判断目标 | 依据 |
| --- | --- |
| 调用是否成功 | 响应体 `code == 200` |
| 某个日期有没有排班 | 该条目是否存在 `shiftName` 键 |

缺省可以出现在窗口内的任意位置。本次是窗口末尾 11月1日至4日，因为排班还没排到那些日子；一旦当天本身没有排班，当天条目同样只有 `date` 和 `remark`。

下游按此处理：某天缺 `shiftName` → 该天无排班数据，设备台历页走无排班的空白状态；缺字段不代表拉取失败，页面不显示「获取失败」。「获取失败」只用于没有拿到响应、业务码非 200、或响应无法解析且无可用缓存的情形。详见 `docs/03` 第 2.2 节。

## 6. 记录的范围

第 4 节和第 5 节的请求头来自本次脚本日志，列出了脚本显式设置的字段。本次日志没有记录 `requests` 自动添加的默认请求头、底层 Host 字段和代理隧道数据。

响应头按实际日志完整列出；响应体为 `requests` 解压后的内容。`Content-Length` 保留服务端返回值，脱敏后的显示内容长度会发生变化。

HTTP `Date` 响应头中的 GMT 为 UTC+0，北京时间为 UTC+8。`09:40:56 GMT` 对应北京时间 `17:40:56`。

## 7. PowerShell 独立调用

下面的命令中 `<client-id>`、`<client-secret>`、`<project-id>`、`<product-id>` 为脱敏占位符，执行前替换成 `tools/agc_probe.py` 里的真实值。

### 7.1 获取令牌并保存到独立文件

以下命令独立发送真实 OAuth 请求，检查 `access_token`，在 UTF-8 文本文件中追加北京时间和完整令牌。每条记录的第一行为时间，第二行为令牌；令牌不包含 `Bearer ` 前缀。

```powershell
Set-Location 'D:\zectrix\zectrix_yike'
$ErrorActionPreference = 'Stop'
$tokenUrl = 'https://connect-drcn.dbankcloud.cn/agc/apigw/oauth2/v1/token'
$clientId = '<client-id>'
$secureSecret = Read-Host '请输入 Client Secret' -AsSecureString
$clientSecret = [System.Net.NetworkCredential]::new('', $secureSecret).Password
$tokenRequest = @{
    grant_type = 'client_credentials'
    client_id = $clientId
    client_secret = $clientSecret
    useJwt = 0
} | ConvertTo-Json -Compress

$tokenResponse = Invoke-RestMethod -Method Post -Uri $tokenUrl `
    -ContentType 'application/json' -Body $tokenRequest -TimeoutSec 15
if ([string]::IsNullOrWhiteSpace([string]$tokenResponse.access_token)) {
    throw 'OAuth 响应中缺少 access_token，请检查服务端业务返回。'
}

$tokenFile = 'D:\zectrix\zectrix_yike\tools\agc_access_token.txt'
$token = [string]$tokenResponse.access_token
$utf8 = [System.Text.UTF8Encoding]::new($false)
$timestamp = [DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(8)).ToString('yyyy-MM-dd HH:mm:ss zzz')
[System.IO.File]::AppendAllText($tokenFile, "`n[$timestamp]`n$token`n", $utf8)
Write-Output "令牌已保存到 $tokenFile，有效期 $($tokenResponse.expires_in) 秒"
```

该文件已列入项目 `.gitignore`。当前 `agc_probe.py` 每次成功获取令牌都会在文件末尾追加北京时间和令牌；本节 PowerShell 命令采用相同的追加方式。

### 7.2 使用本地令牌调用云函数

以下命令使用第 5 节的分组编号及查询开始日期。请求体文件保存在项目已有忽略规则覆盖的 `.tmp_agc_manual` 目录中。请求正文由文件发送，以保留 JSON 引号。

```powershell
Set-Location 'D:\zectrix\zectrix_yike'
$ErrorActionPreference = 'Stop'
$proxyUrl = 'https://connect-drcn.dbankcloud.cn/agc/apigw/wisefunction/functions/<project-id>/proxy-function-$latest'
$tokenFile = 'D:\zectrix\zectrix_yike\tools\agc_access_token.txt'
$token = Get-Content -LiteralPath $tokenFile | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Last 1
if ([string]::IsNullOrWhiteSpace($token)) {
    throw '本地令牌文件为空。'
}
$secureApiKey = Read-Host '请输入 CUSTOM_API_KEY' -AsSecureString
$apiKey = [System.Net.NetworkCredential]::new('', $secureApiKey).Password
$requestBody = @{
    apiKey = $apiKey
    data = @{
        groupId = '<group-id>'
        startDate = '2026-10-22'
    }
} | ConvertTo-Json -Depth 3 -Compress

$bodyDirectory = 'D:\zectrix\zectrix_yike\.tmp_agc_manual'
New-Item -ItemType Directory -Path $bodyDirectory -Force | Out-Null
$bodyFile = Join-Path $bodyDirectory 'function-request.json'
$utf8 = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText($bodyFile, $requestBody, $utf8)

curl.exe --request POST $proxyUrl `
    --header 'clientId: <client-id>' `
    --header "Authorization: Bearer $token" `
    --header 'productId: <product-id>' `
    --header 'Content-Type: application/json' `
    --data-binary "@$bodyFile" --include --max-time 15
if ($LASTEXITCODE -ne 0) {
    throw "curl 调用失败，退出码为 $LASTEXITCODE。"
}
```

`--include` 输出 HTTP 状态、响应头和响应体。使用该命令时仍需检查返回的 HTTP 状态，curl 成功完成传输时可能收到 HTTP 403。

本节命令使用 PowerShell 与 curl 各自的网络配置；如需指定本地 HTTP 代理，可在 `Invoke-RestMethod` 中加入 `-Proxy 'http://127.0.0.1:7897'`，并在 curl 命令中加入 `--proxy 'http://127.0.0.1:7897'`。

## 8. 提供给后端定位的信息

| 项目 | 本次记录 |
| --- | --- |
| 调用时间 | 北京时间 `2026-10-22 17:40` |
| 服务端响应时间 | `Thu, 22 Oct 2026 09:40:56 GMT` |
| OAuth 结果 | HTTP 200，返回非空令牌 |
| OAuth useJwt | 数字 `0` |
| OAuth 令牌长度 | 227 个字符 |
| 云函数响应 | HTTP 200，业务码 200，查询成功 |
| 云函数响应体 | 完整内容见第 5.4 节，包含 14 条日期数据 |
| 查询参数 | `groupId=<group-id>`，`startDate=2026-10-22` |
| Client ID | `<client-id>` |
| 请求头 productId | `<product-id>`，待控制台核对 |
| 函数名称 | URL 中为 `proxy-function` |
| URL 版本标记 | `$latest` |
| authType | 待提供控制台当前配置 |
| 部署状态与实际绑定版本 | 待提供控制台当前配置 |

待补充材料：

1. AGC 控制台“项目设置 > 常规”截图，Client Secret 脱敏。
2. 触发器配置、authType、部署状态及函数版本截图。
3. `agconnect-services.json` 脱敏内容。
4. 后端同一时间范围内的鉴权日志，以及可用的请求关联编号。

## 9. 固件侧实现（2026年10月26日）

### 9.1 代码位置

| 环节 | 文件 |
| --- | --- |
| 响应解析、缓存编解码、Date 头换算（纯 C，零 IDF 依赖） | `app/components/cal_sched/cal_sched_parse.c` |
| 两步 HTTPS 调用与重试 | `app/components/cal_sched/cal_sched_http.cc` |
| 排班缓存 NVS 读写 | `app/components/cal_sched/cal_sched_store.cc` |
| 取数凭证 NVS 读写 | `app/components/cal_net/cal_cred.cc` |
| 接入每日唤醒与校时 | `app/main/app_main.cc`（`RenderTodayPage`、`ApplyServerTime`） |

### 9.2 与本文档请求的对应关系

| 项 | PC 侧 `tools/agc_probe.py` | 固件 |
| --- | --- | --- |
| OAuth 地址 | `TOKEN_URL` | 同一地址，硬编码 |
| 云函数地址 | 生效值 `PROXY_URL_ORI` | 同一地址，硬编码；`connect-api.cloud.huawei.com` 不用 |
| `useJwt` | 数字 `0` | 数字 `0` |
| 云函数请求头 | `clientId` / `productId` / `Authorization` | 同三项，值相同 |
| 超时 | 15 秒 | 15 秒 |
| 证书校验 | `requests` 默认校验 | `esp_crt_bundle_attach`（`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`），不跳过校验 |
| `startDate` | 留空时取北京时间当天 | 取 RTC 当天日期，格式 `YYYY-MM-DD` |
| 业务成功判据 | `code == 200` | `code == 200`，否则判失败并打出 `message` |

凭证（Client ID / Client Secret / apiKey / groupId）不编进固件，存放在 NVS 命名空间 `cal_agc`，由配网写入，见 `docs/12` 第 2 节。`productId` 是网关路由头、不是密钥，跟着接口地址写在代码里。

### 9.3 重试与超时

| 情形 | 处置 |
| --- | --- |
| 取数失败（token、云函数、解析任一环节） | 间隔 60 秒重试，最多 3 轮（`docs/03` §2.4） |
| WiFi 关联失败 | 立即进下一轮，不额外等 60 秒 —— 关联本身已耗掉 10 秒并做过退避，再等只是白烧电 |
| 响应体超过 4096 字节 | 判失败，不截断。截断会让 JSON 断在某条记录中间，看起来像“少了一天” |
| 凭证不齐 | 跳过取数，页面按「获取失败」绘制，日志打印缺哪几项 |

### 9.4 访问令牌复用

`expires_in` 实测 **175480 秒**（约 48.7 小时），设备一天醒一次 —— 每次唤醒都换令牌等于把一次 TLS 握手变成两次，唤醒时长与耗电都翻倍。

令牌连同**绝对过期时刻**（本地时间 + `expires_in`）存进 NVS `cal_agc`，键 `token` 与 `expire`。每次取数前先判定：

| 情形 | 动作 |
| --- | --- |
| 过期时刻 − 3600 秒 > 当前时间 | 直接用缓存令牌，**跳过 OAuth**（日志：`复用缓存令牌，还剩 N 秒`） |
| 已过期或没有缓存 | 换新令牌并写回 NVS（日志：`令牌已缓存，过期时刻 …`） |
| 云函数返回 401 | 清掉缓存令牌 → 换新的 → 重试一次；仍 401 就是 Client ID / Secret 本身有问题 |
| 配网重写凭证 | 一并清掉令牌 —— 旧令牌是按旧凭证换来的，继续用要么 401，要么拿到别人的数据 |

1 小时余量吸收两类边界：本地时钟刚校准或尚未校准，以及请求在路上的往返时间 —— 拿一个只剩 30 秒的令牌去调用，响应回来时它可能已经失效，白跑一轮。

时钟被重置到 1970 时本地判据一定说「还没过期」，这种情况不在这里判死，交给 401 兜底纠正。

### 9.5 缓存与三态判定

拉到的是**今天起的 14 天滚动窗口**（`docs/03` §6.2）。整份缓存序列化成 1108 字节的 blob 存 NVS 命名空间 `cal_sched`、键 `days`，每天覆盖一次。

页面按三态绘制（`docs/03` §2.2）：

| 缓存里有没有今天 | 今天的 `shiftName` | 页面 |
| --- | --- | --- |
| 有 | 存在 | 正常显示班次与两段时段 |
| 有 | 被省略 | 主区留白、底栏不画班次块（该天确实没有排班） |
| 无 | — | 主区「获取失败」、底栏不画块（本次没取到、本地也没有历史数据） |

休息日的 `amStart`/`pmStart` 虽然服务端给的是 `00:00`，解析阶段直接丢弃 —— 照抄会画出「凌晨00:00-00:00」。班次名保留，底栏的反白块要显示「休」。

### 9.6 RTC 校准

每次成功取数都用云函数响应头的 `Date` 校准 PCF8563（`docs/03` §6.1）：解析 RFC 1123 时间 → UTC 秒 → 加 8 小时得北京时间 → 与 RTC 当前值比较，**差异超过 5 秒才写**。RTC 每天误差约 1.7 秒，每天写一次就够。

⚠️ **`esp_http_client_get_header(client, "Date", …)` 取不到这个头**，实测返回空值 —— 那个接口只覆盖驱动内部记下来的少数几个头，`Date` 不在其中。必须挂 `config.event_handler`，在 `HTTP_EVENT_ON_HEADER` 回调里认 `header_key == "Date"`。第一版就是踩在这里：链路全程 HTTP 200、数据全对，只有日志里一句 `Date 头不可用（""）`。

校时后日期可能变（RTC 慢到前一天 23:5x 又恰好当天唤醒），因此出页用的日期以校时后的值为准，取数不会因此重跑一轮。

### 9.7 验证

| 层 | 手段 | 结果 |
| --- | --- | --- |
| 接口 | 2026-10-26 重跑 `tools/agc_probe.py`：HTTP 200，14 条，窗口 `2026-10-26` → `2026-11-08`，契约检查通过 | 通过 |
| PC | `tools/sched_parse_test.py`：用本文档 §5.4 的真实响应跑 181 项断言（含 6 种违约样本、缓存往返、Date 头跨日边界、令牌有效期判据） | 0 失败 |
| 板端 | `app_main.cc::RunSchedSelfCheck`：同一份真实响应 + 缓存往返 + Date 头 + 令牌判据，不需网络 | 见串口日志 `sched self-check: ... 0 mismatch` |
| 真机 | 每日唤醒时的两步调用与页面显示 | 见 §9.8 |

### 9.8 真机实测（2026-10-26）

设备 MAC `<device-mac>`，COM5，921600。凭证通过串口配网写入（`PROV {"ok":true,"agcComplete":true}`）。

| 项 | 结果 | 证据 |
| --- | --- | --- |
| 配网写凭证 | 通过 | 回执 `{"ok": true, "agcComplete": true}`，重启后不再进配网 |
| 启动自检 | 通过 | `sched self-check: 6 条样本、缓存往返、Date 头、令牌有效期，0 mismatch` |
| OAuth | 通过 | `OAuth：HTTP 200，耗时 1722 ms`，令牌 227 字符，有效期 **176413** 秒 |
| 令牌缓存 | 通过 | `令牌已缓存，过期时刻 1793161717（本地时间）` |
| 云函数 | 通过 | `HTTP 200，耗时 2126–5066 ms，响应 1136 字节`，14 天 |
| 复用令牌 | 通过 | `复用缓存令牌，还剩 176384 秒（49.0 小时），跳过 OAuth` |
| 取数总耗时 | 8710 ms → **4105 ms** | 第二次唤醒省掉整个 OAuth 环节及其 TLS 握手 |
| Date 校时 | 通过 | 首次 `RTC 已校准，偏差 56 秒`；随后 `相差 0 秒` / `1 秒` 不写 |
| 页面 | 通过 | `班次 B1 [09:30-13:15] [17:00-21:00]` |
| 收尾 | 通过 | `deep sleep until 2026-10-27 00:00:00` |

`expires_in` 两次实测不同（175480 / 176413），因此绝对过期时刻必须用每次响应的值算，不能写死。

待补：断网兜底（取数失败后用缓存绘制）与「获取失败」画面的真机验证 —— 需要临时把 `apiKey` 写错再唤醒一次。
