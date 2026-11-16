@echo off
chcp 65001 >nul
setlocal
title 微信轻聊 本机 Windows 测试版
pushd "%~dp0"
if errorlevel 1 (
    echo 无法打开共享测试目录。
    pause
    exit /b 1
)

echo 测试目录：%CD%
echo.
for %%P in (wechat-relay.exe wechat-adapter.exe wechat-server.exe) do (
    tasklist /FI "IMAGENAME eq %%P" 2>nul | find /I "%%P" >nul
    if not errorlevel 1 (
        echo 检测到 %%P 正在运行。请从服务端托盘选择“停止服务并退出”，再启动测试版。
        goto :failed
    )
)
if not exist "wechat-server.exe" goto :missing
if not exist "adapter\ffmpeg.exe" goto :missing
if not exist "测试构建.json" goto :missing

powershell -NoProfile -Command "try { $m = Get-Content -LiteralPath '测试构建.json' -Raw -Encoding UTF8 | ConvertFrom-Json; foreach ($f in $m.files) { if ((Get-FileHash -LiteralPath $f.path -Algorithm SHA256).Hash -ne $f.sha256) { throw ('文件校验失败：' + $f.path) } }; Write-Host ('功能版本：' + $m.feature); Write-Host ('适配器 SHA-256：' + $m.files[0].sha256) } catch { Write-Host $_; exit 1 }"
if errorlevel 1 goto :failed
echo.
echo 启动后在服务端日志核对：image-preview=cdn-footer-v3 media-wait=delayed-clear-v1 inbox-poll=snapshot-v1
echo 微信需保持登录。关闭微信主窗口时，保留 Weixin.exe 进程。
echo 完整图片暂不可用时先显示缩略图，完整图取得后更新原消息；步骤见本目录 使用说明.md。
echo.
set "TEST_CONFIG=%LOCALAPPDATA%\WeChatLite\WeChatLiteServer\server.json"
if exist "%TEST_CONFIG%" (
    "wechat-server.exe" --config "%TEST_CONFIG%" --start
) else (
    "wechat-server.exe" --start
)
popd
exit /b

:missing
echo 测试目录缺少必要文件，请重新交付完整测试目录。
:failed
echo 未启动服务；没有强制结束任何进程。
pause
popd
exit /b 1
