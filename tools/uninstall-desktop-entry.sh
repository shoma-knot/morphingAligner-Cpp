#!/usr/bin/env sh
# tools/install-desktop-entry.sh で作ったものを取り消す。
# リポジトリの外に出ているのはシンボリックリンク1個だけなので、これを消せば
# あとはこのディレクトリごと削除してよい。
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
NAME=morphingaligner
DESKTOP="$ROOT/$NAME.desktop"
LINK_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
LINK="$LINK_DIR/$NAME.desktop"

if [ -L "$LINK" ]; then
    # 自分が張ったリンクだけを消す（同名の別物を巻き込まないため）。
    TARGET=$(readlink "$LINK")
    if [ "$TARGET" = "$DESKTOP" ]; then
        rm -f "$LINK"
        echo "リンクを削除しました: $LINK"
    else
        echo "別の場所を指すリンクなので残します: $LINK -> $TARGET" >&2
    fi
elif [ -e "$LINK" ]; then
    echo "シンボリックリンクではないので残します: $LINK" >&2
else
    echo "リンクはありません: $LINK"
fi

rm -f "$DESKTOP"

if command -v update-desktop-database > /dev/null 2>&1; then
    update-desktop-database "$LINK_DIR" 2> /dev/null || true
fi

echo "解除しました。あとはこのディレクトリを削除すれば残りません: $ROOT"
