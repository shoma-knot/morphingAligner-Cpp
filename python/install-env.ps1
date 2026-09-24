# morphingAligner の音声解析（フォルマント推定・音素アライメント）用の Python 環境を作る。
# install-win.bat から呼ばれる。配布物のルート（このスクリプトの1つ上）に .env を作る。
#
# conda 系のツール（micromamba / mamba / conda）はユーザーが用意する。見つからなければ
# 入れ方を案内して終了する（こちらでダウンロード・同梱はしない）。
#
# 引数:
#   -Yes   確認を省く（CI などの非対話実行用）
#
# ※ このファイルは UTF-8（BOM 付き）で保存すること。Windows PowerShell 5.1 は BOM の無い
#   UTF-8 を ANSI（Shift_JIS）として読み、日本語の文字列が化ける。
param([switch]$Yes)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$Root    = Split-Path -Parent $PSScriptRoot
$EnvDir  = Join-Path $Root '.env'
$EnvFile = Join-Path $PSScriptRoot 'environment.yml'
$Tools   = Join-Path $PSScriptRoot 'speech_tools.py'
$Python  = Join-Path $EnvDir 'python.exe'

function Confirm-Step([string]$Message) {
    if ($Yes) { return $true }
    $answer = Read-Host "$Message [Y/n]"
    return ($answer -eq '' -or $answer -match '^[Yy]')
}

# micromamba → mamba → conda の順に探す（PATH と、各ツールのシェル初期化が設定する環境変数）。
function Find-CondaTool {
    foreach ($name in 'micromamba', 'mamba', 'conda') {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($cmd) { return @{ Name = $name; Path = $cmd.Source } }
    }
    foreach ($var in 'MAMBA_EXE', 'CONDA_EXE') {
        $p = [Environment]::GetEnvironmentVariable($var)
        if ($p -and (Test-Path $p)) {
            return @{ Name = [IO.Path]::GetFileNameWithoutExtension($p); Path = $p }
        }
    }
    return $null
}

Write-Host '== morphingAligner: 音声解析の環境を作ります =='
Write-Host "作成先: $EnvDir"
Write-Host '約 5 GB の空き容量（環境・パッケージのキャッシュ・モデル）とインターネット接続が必要です。'
Write-Host '初回は 10 分以上かかることがあります。'
if (-not ($Root -match '^[\x00-\x7F]*$')) {
    Write-Warning 'このフォルダのパスに ASCII 以外の文字が含まれています。音素アライメントが失敗することがあります。'
}

$tool = Find-CondaTool
if (-not $tool) {
    Write-Host ''
    Write-Host 'conda 系のツール（micromamba / mamba / conda）が見つかりません。'
    Write-Host 'micromamba を入れてから、新しいターミナルでこのスクリプトを実行し直してください。'
    Write-Host '  入れ方（PowerShell）:'
    Write-Host '    Invoke-Expression ((Invoke-WebRequest -Uri https://micro.mamba.pm/install.ps1 -UseBasicParsing).Content)'
    Write-Host '  詳細: https://mamba.readthedocs.io/en/latest/installation/micromamba-installation.html'
    Write-Host '（Miniforge の conda / mamba でも構いません: https://conda-forge.org/download/ ）'
    exit 1
}
Write-Host "使うツール: $($tool.Name)（$($tool.Path)）"

# 既存の環境: 同じ定義で作ったものなら作り直さない。違えば確認のうえ作り直す。
$needCreate = $true
if (Test-Path $EnvDir) {
    $same = $false
    if (Test-Path $Python) {
        & $Python $Tools --check-marker | Out-Null
        $same = ($LASTEXITCODE -eq 0)
    }
    if ($same) {
        Write-Host '環境は作成済みで、定義も最新です（作り直しません）。'
        $needCreate = $false
    } elseif (Confirm-Step '既存の環境（.env）は今の定義で作られたものではありません（古い・壊れている・手で作った等）。削除して作り直しますか？') {
        Remove-Item -Recurse -Force $EnvDir
    } else {
        Write-Host '中止しました。'
        exit 1
    }
}

if ($needCreate) {
    if (-not (Confirm-Step '環境を作成します。続けますか？')) { Write-Host '中止しました。'; exit 1 }
    # チャンネルは environment.yml の conda-forge のみ（nodefaults）。Anaconda の defaults は使わない。
    $createArgs = @('env', 'create', '-p', $EnvDir, '-f', $EnvFile)
    if ($tool.Name -ne 'conda') { $createArgs += '-y' }
    & $tool.Path @createArgs
    if ($LASTEXITCODE -ne 0) { Write-Host '環境の作成に失敗しました。'; exit 1 }
}

Write-Host '== MFA の日本語モデルを取得します =='
& $Python $Tools --download-models
if ($LASTEXITCODE -ne 0) { Write-Host 'モデルの取得に失敗しました。'; exit 1 }

& $Python $Tools --write-marker
Write-Host '== 環境を確認します =='
& $Python $Tools --check
if ($LASTEXITCODE -ne 0) { Write-Host '環境に問題があります（上の [問題] を参照）。'; exit 1 }
Write-Host '完了しました。morphingAligner を起動し直すと、フォルマント表示と音素アライメントが使えます。'
exit 0
