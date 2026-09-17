#!/usr/bin/env sh
# デスクトップエントリを登録して、タスクバーにアイコンを出す。
#
# GNOME Shell はウィンドウを WM_CLASS で .desktop に紐づけ、そこに書かれたアイコンを
# 表示する（glfwSetWindowIcon で設定した _NET_WM_ICON は見ない）。そのため登録自体は
# 避けられないが、外に出るものはシンボリックリンク1個だけにしてある:
#
#   ~/.local/share/applications/morphingaligner.desktop  →  リポジトリ内の実体
#
# アイコンは Icon= に絶対パスを書くのでアイコンテーマへの設置は不要。
# 解除は tools/uninstall-desktop-entry.sh を実行するだけ。
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
NAME=morphingaligner
DESKTOP="$ROOT/$NAME.desktop"
EXEC="$ROOT/bin/main"
ICON="$ROOT/icon/icon_256.png"
LINK_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
LINK="$LINK_DIR/$NAME.desktop"

if [ ! -x "$EXEC" ]; then
    echo "実行ファイルがありません: $EXEC" >&2
    echo "先にビルドしてください（cmake --build build）" >&2
    exit 1
fi
if [ ! -f "$ICON" ]; then
    echo "アイコンがありません: $ICON" >&2
    echo "tools/gen_icon.py で生成できます" >&2
    exit 1
fi

# Path= は作業ディレクトリ。これがないとランチャーから起動したときの作業ディレクトリが
# ホームになり、font/ や licenses/ を相対パスで探している箇所が失敗する（日本語フォントが
# 読めず文字化けする）。
# StartupWMClass= は実行中ウィンドウとの紐づけ。値は WM_CLASS と一致させること。
# NoDisplay=true はアプリ一覧に出さない指定。エントリ自体はデスクトップデータベースに
# 残るので、ウィンドウとの紐づけ（＝タスクバーのアイコン）はそのまま働く。
cat > "$DESKTOP" <<DESKTOP_EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=morphingAligner
Comment=Speech morphing with WORLD and tcmorph
Comment[ja]=WORLD と tcmorph による音声モーフィング
Exec=$EXEC
Path=$ROOT
Icon=$ICON
Terminal=false
Categories=AudioVideo;Audio;
StartupWMClass=morphingAligner
StartupNotify=false
NoDisplay=true
DESKTOP_EOF

if command -v desktop-file-validate > /dev/null 2>&1; then
    desktop-file-validate "$DESKTOP" || {
        echo "生成した .desktop が仕様に適合しません: $DESKTOP" >&2
        exit 1
    }
fi

mkdir -p "$LINK_DIR"
if [ -e "$LINK" ] && [ ! -L "$LINK" ]; then
    echo "同名の実ファイルが既にあります。手動で確認してください: $LINK" >&2
    exit 1
fi
ln -sfn "$DESKTOP" "$LINK"

if command -v update-desktop-database > /dev/null 2>&1; then
    update-desktop-database "$LINK_DIR" 2> /dev/null || true
fi

echo "登録しました"
echo "  実体: $DESKTOP"
echo "  リンク: $LINK"
echo "アイコンが変わらない場合は、アプリを起動し直すか再ログインしてください。"
