<#
    idf.ps1 —— ESP-IDF 构建入口（PowerShell 薄壳）

    真正的环境构造全部交给 tools/idf.py（Python 侧显式构造 env 传给子进程）。

    为什么不在这里设环境变量：
      Windows 进程环境块里 'Path' / 'PATH' 可能并存。实测在 PowerShell 里
      `$env:Path = ...` 之后，子进程读到的仍是旧 PATH（普通变量能传，PATH 被还原），
      于是 cmake 报 "unable to find a build program corresponding to Ninja"。
      Python 的 subprocess(env=...) 不受该问题影响，所以统一走 idf.py。

    用法:
      powershell -File tools\idf.ps1 build
      powershell -File tools\idf.ps1 set-target esp32s3
#>
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs
)

$launcher = "C:\Users\sfuit\.workbuddy\binaries\python\versions\3.13.12\python.exe"
$entry = "D:\zectrix\zectrix_yike\tools\idf.py"

if (-not (Test-Path $launcher)) {
    # 退而求其次：用 IDF 自带 Python 作为外层启动器
    $launcher = "D:\development_tools\esp\espressif\python_env\idf5.4_py3.13_env\Scripts\python.exe"
}

& $launcher $entry @IdfArgs
exit $LASTEXITCODE
