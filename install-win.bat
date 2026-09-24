@echo off
rem morphingAligner の音声解析（フォルマント推定・音素アライメント）用の Python 環境を作る（Windows）。
rem ダブルクリックで実行できるよう、実体の python\install-env.ps1 をここから呼び出す。
rem 引数はそのまま渡す（例: install-win.bat -Yes で確認を省く）。
rem 環境変数 MORPHALIGNER_NO_PAUSE が設定されていれば、最後に一時停止しない（CI 用）。
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0python\install-env.ps1" %*
set RC=%ERRORLEVEL%
if not defined MORPHALIGNER_NO_PAUSE pause
exit /b %RC%
