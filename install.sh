#!/usr/bin/env sh
# morphingAligner の音声解析（フォルマント推定・音素アライメント）用の Python 環境を作る（Linux）。
# 配布物のルート（このスクリプトの場所）に .env を作る。
#
# conda 系のツール（micromamba / mamba / conda）はユーザーが用意する。見つからなければ
# 入れ方を案内して終了する（こちらでダウンロード・同梱はしない）。
#
# 使い方: ./install.sh [-y]   （-y で確認を省く。CI などの非対話実行用）
set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
ENV_DIR="$ROOT/.env"
ENV_FILE="$ROOT/python/environment.yml"
TOOLS="$ROOT/python/speech_tools.py"
PYTHON="$ENV_DIR/bin/python"
YES=0
[ "${1:-}" = "-y" ] && YES=1

confirm() {
    [ "$YES" -eq 1 ] && return 0
    printf '%s [Y/n] ' "$1"
    read -r answer || answer=n
    case "$answer" in "" | [Yy]*) return 0 ;; *) return 1 ;; esac
}

# micromamba → mamba → conda の順に探す（PATH と、各ツールのシェル初期化が設定する環境変数）。
find_tool() {
    for name in micromamba mamba conda; do
        if command -v "$name" > /dev/null 2>&1; then
            TOOL_NAME=$name
            TOOL=$(command -v "$name")
            return 0
        fi
    done
    for p in "${MAMBA_EXE:-}" "${CONDA_EXE:-}"; do
        if [ -n "$p" ] && [ -x "$p" ]; then
            TOOL_NAME=$(basename "$p")
            TOOL=$p
            return 0
        fi
    done
    return 1
}

echo "== morphingAligner: 音声解析の環境を作ります =="
echo "作成先: $ENV_DIR"
echo "約 5 GB の空き容量（環境・パッケージのキャッシュ・モデル）とインターネット接続が必要です。"
echo "初回は 10 分以上かかることがあります。"
if printf '%s' "$ROOT" | LC_ALL=C grep -q '[^ -~]'; then
    echo "警告: このフォルダのパスに ASCII 以外の文字が含まれています。音素アライメントが失敗することがあります。"
fi

if ! find_tool; then
    echo ""
    echo "conda 系のツール（micromamba / mamba / conda）が見つかりません。"
    echo "micromamba を入れてから、新しいシェルでこのスクリプトを実行し直してください。"
    echo "  入れ方:"
    echo "    \"\${SHELL}\" <(curl -L micro.mamba.pm)"
    echo "  詳細: https://mamba.readthedocs.io/en/latest/installation/micromamba-installation.html"
    echo "（Miniforge の conda / mamba でも構いません: https://conda-forge.org/download/ ）"
    exit 1
fi
echo "使うツール: $TOOL_NAME（$TOOL）"

# 既存の環境: 同じ定義で作ったものなら作り直さない。違えば確認のうえ作り直す。
NEED_CREATE=1
if [ -d "$ENV_DIR" ]; then
    if [ -x "$PYTHON" ] && "$PYTHON" "$TOOLS" --check-marker > /dev/null 2>&1; then
        echo "環境は作成済みで、定義も最新です（作り直しません）。"
        NEED_CREATE=0
    elif confirm "既存の環境（.env）は今の定義で作られたものではありません（古い・壊れている・手で作った等）。削除して作り直しますか？"; then
        rm -rf "$ENV_DIR"
    else
        echo "中止しました。"
        exit 1
    fi
fi

if [ "$NEED_CREATE" -eq 1 ]; then
    confirm "環境を作成します。続けますか？" || { echo "中止しました。"; exit 1; }
    # チャンネルは environment.yml の conda-forge のみ（nodefaults）。Anaconda の defaults は使わない。
    if [ "$TOOL_NAME" = "conda" ]; then
        "$TOOL" env create -p "$ENV_DIR" -f "$ENV_FILE" || { echo "環境の作成に失敗しました。"; exit 1; }
    else
        "$TOOL" env create -y -p "$ENV_DIR" -f "$ENV_FILE" || { echo "環境の作成に失敗しました。"; exit 1; }
    fi
fi

echo "== MFA の日本語モデルを取得します =="
"$PYTHON" "$TOOLS" --download-models || { echo "モデルの取得に失敗しました。"; exit 1; }

"$PYTHON" "$TOOLS" --write-marker
echo "== 環境を確認します =="
"$PYTHON" "$TOOLS" --check || { echo "環境に問題があります（上の [問題] を参照）。"; exit 1; }
echo "完了しました。morphingAligner を起動し直すと、フォルマント表示と音素アライメントが使えます。"
