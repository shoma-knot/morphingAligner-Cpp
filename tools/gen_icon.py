#!/usr/bin/env python3
"""morphingAligner のアプリアイコンを生成する。

    python3 tools/gen_icon.py

icon/ に PNG と Windows 用の .ico を、src/app_icon_data.inc にウィンドウアイコン用の
RGBA を書き出す。
意匠はアプリ画面と同じ語彙（暗い背景・viridis のスペクトログラム・橙のアンカー）で、
「長さの違う2つの発話を、アンカーで時間対応づける」ことを表す。

依存: Pillow（pip install pillow）。生成物はリポジトリに入れてあるので、
意匠を変えたいときだけ実行すればよい。
"""
from PIL import Image, ImageDraw
import pathlib
import struct

SS     = 2048    # 描画解像度。各サイズへ縮小することでアンチエイリアスする
BG     = (0x17, 0x17, 0x1C, 255)    # 背景（アプリのクリアカラーに合わせた暗色）
ANCHOR = (0xFF, 0x59, 0x33, 255)    # アンカー（anchors.cpp の kAnchorCol と同じ）

# viridis の中〜高域。暗紫は背景に沈むので使わない
VIRIDIS = [(0x3B, 0x52, 0x8B), (0x21, 0x91, 0x8C), (0x5E, 0xC9, 0x62), (0xFD, 0xE7, 0x25)]

BAND_H       = 0.195            # 帯の高さ（キャンバスに対する比）
Y_TOP, Y_BOT = 0.325, 0.675     # 上下の帯の中心
SIZES        = (16, 32, 48, 64)    # ウィンドウアイコンに埋め込むサイズ
PNG_SIZES    = (256, 128, 64, 48, 32, 16)
ICO_SIZES    = (16, 24, 32, 48, 64, 128, 256)    # .ico に入れるサイズ
# .ico の中でこのサイズ以下は BMP、それより大きいものは PNG で格納する。
# PNG 形式のエントリは Vista 以降しか扱えず、小サイズでの扱いが実装依存になりがち
# なので、慣例どおり大サイズだけ PNG にする。
ICO_PNG_MIN  = 128


def viridis(t):
    t = min(max(t, 0.0), 1.0) * (len(VIRIDIS) - 1)
    i = min(int(t), len(VIRIDIS) - 2)
    f = t - i
    a, b = VIRIDIS[i], VIRIDIS[i + 1]
    return tuple(int(a[k] + (b[k] - a[k]) * f) for k in range(3)) + (255,)


def band(x0, x1, yc, t0, t1):
    """viridis を t0..t1 で変化させた角丸の帯（スペクトログラムの一片）。"""
    x0, x1, yc, h = x0 * SS, x1 * SS, yc * SS, BAND_H * SS
    layer = Image.new("RGBA", (SS, SS), (0, 0, 0, 0))
    ld    = ImageDraw.Draw(layer)
    for x in range(int(x0), int(x1)):    # 1px の縦ストライプでグラデーションを作る
        ld.rectangle([x, yc - h / 2, x + 1, yc + h / 2],
                     fill=viridis(t0 + (t1 - t0) * (x - x0) / (x1 - x0)))
    mask = Image.new("L", (SS, SS), 0)
    ImageDraw.Draw(mask).rounded_rectangle([x0, yc - h / 2, x1, yc + h / 2],
                                           radius=int(0.022 * SS), fill=255)
    layer.putalpha(mask)
    return layer


def anchor(dr, xt, xb, w):
    """上帯から下帯へ通す1本のアンカー。帯の中は垂直、帯の間は傾けて時間のずれを表す。"""
    h = BAND_H * SS / 2 + 0.012 * SS    # 帯からわずかにはみ出させる
    xt, xb, yt, yb = xt * SS, xb * SS, Y_TOP * SS, Y_BOT * SS
    dr.line([(xt, yt - h), (xt, yt + h), (xb, yb - h), (xb, yb + h)],
            fill=ANCHOR, width=int(w * SS), joint="curve")


def render():
    img = Image.new("RGBA", (SS, SS), (0, 0, 0, 0))
    dr  = ImageDraw.Draw(img)
    dr.rounded_rectangle([0, 0, SS - 1, SS - 1], radius=int(0.22 * SS), fill=BG)
    # base（上）は短く target（下）は長い＝発話長が違う、という含み
    img.alpha_composite(band(0.12, 0.74, Y_TOP, 0.10, 1.00))
    img.alpha_composite(band(0.24, 0.88, Y_BOT, 0.00, 0.88))
    anchor(dr, 0.29, 0.38, 0.034)
    anchor(dr, 0.58, 0.70, 0.034)
    return img


def write_inc(img, path):
    """glfwSetWindowIcon にそのまま渡せる RGBA を C++ の配列として書き出す。"""
    out = ['// このファイルは tools/gen_icon.py が生成する。手で編集しないこと。',
           '// glfwSetWindowIcon に渡す RGBA 画素（行優先・各画素 R,G,B,A の 4 バイト）。',
           '']
    for s in SIZES:
        px = list(img.resize((s, s), Image.LANCZOS).convert("RGBA").getdata())
        out.append(f'static const unsigned char kIcon{s}[{s} * {s} * 4] = {{')
        flat  = [c for p in px for c in p]
        lines = [''.join(f'0x{b:02x},' for b in flat[i:i + 16]) for i in range(0, len(flat), 16)]
        out += ['    ' + ln for ln in lines]
        out += ['};', '']
    out.append(f'static const int kIconSizes[] = {{ {", ".join(str(s) for s in SIZES)} }};')
    out.append(f'static const unsigned char* const kIconPixels[] = {{ {", ".join("kIcon" + str(s) for s in SIZES)} }};')
    out.append('')
    path.write_text('\n'.join(out))


def ico_bmp_entry(im):
    """.ico に入れる BMP（DIB）形式の1エントリを作る。"""
    s = im.size[0]
    # XOR 画像は BGRA のボトムアップ。
    px  = im.load()
    xor = b''.join(bytes(v for x in range(s) for v in (lambda r, g, b, a: (b, g, r, a))(*px[x, y]))
                   for y in range(s - 1, -1, -1))
    # AND マスクは 1bpp で各行を4バイト境界に揃える。32bit アイコンでは XOR 側の
    # アルファが使われるので中身は全0でよいが、形式上は必要。
    and_mask = b'\x00' * (((s + 31) // 32) * 4 * s)
    # BITMAPINFOHEADER。高さは XOR と AND を合わせた 2 倍を書く決まり。
    header = struct.pack('<IiiHHIIiiII', 40, s, s * 2, 1, 32, 0,
                         len(xor) + len(and_mask), 0, 0, 0, 0)
    return header + xor + and_mask


def write_ico(img, path):
    """複数サイズをまとめた Windows の .ico を書き出す。"""
    entries = []
    for s in sorted(ICO_SIZES):
        frame = img.resize((s, s), Image.LANCZOS).convert('RGBA')
        if s >= ICO_PNG_MIN:
            buf = __import__('io').BytesIO()
            frame.save(buf, format='PNG')
            entries.append((s, buf.getvalue()))
        else:
            entries.append((s, ico_bmp_entry(frame)))

    offset = 6 + 16 * len(entries)    # ICONDIR + ICONDIRENTRY の合計
    dir_bytes = struct.pack('<HHH', 0, 1, len(entries))
    for s, data in entries:
        # 256px は幅・高さのバイトに 0 を書く決まり。
        b = 0 if s == 256 else s
        dir_bytes += struct.pack('<BBBBHHII', b, b, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    path.write_bytes(dir_bytes + b''.join(d for _, d in entries))


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    img  = render()
    (root / 'icon').mkdir(exist_ok=True)
    for s in PNG_SIZES:
        img.resize((s, s), Image.LANCZOS).save(root / 'icon' / f'icon_{s}.png')
    write_ico(img, root / 'icon' / 'morphingaligner.ico')
    write_inc(img, root / 'src' / 'app_icon_data.inc')
    print(f'icon/*.png, icon/morphingaligner.ico, src/app_icon_data.inc を書き出しました')
    print(f'  ウィンドウアイコン埋め込み: {SIZES}')
    print(f'  .ico: {ICO_SIZES}（{ICO_PNG_MIN}px 以上は PNG 格納）')


if __name__ == '__main__':
    main()
