#!/usr/bin/env python3
"""
compare_audio.py

MATLAB 版 GUI が書き出した WAV と、C++ 版が合成した WAV を比較する。
MATLAB 環境が無くても実行できる。

    python compare_audio.py out/ --matlab "matlab_wav/*.wav"

--------------------------------------------------------------------------
なぜ波形を直接比べないのか
--------------------------------------------------------------------------
WORLD の合成は非周期成分に乱数を使うため、パラメータが完全に同一でも
波形はサンプル単位では一致しない。さらに MATLAB 版 GUI の合成器と
pyworld は別実装なので、その差も乗る。

そこで波形ではなく、両者を再分析した特徴量（F0 とスペクトル包絡）で
比較する。ただし再分析自体も誤差を生むので、「一致していても出てしまう
誤差の水準」を同時に測らないと数値を解釈できない。

--------------------------------------------------------------------------
3 つの対照条件
--------------------------------------------------------------------------
本スクリプトは測定値と併せて以下を出す。これが無いと合否を判断できない。

  [床] 分析再合成の往復誤差
      C++ の WAV を分析して再合成し、元の C++ WAV と比べた距離。
      合成と分析の鎖そのものが生む誤差なので、測定値がこれと同程度なら
      モーフィングの差は検出限界以下ということになる。

  [較正] 端点 (alpha=0, 1) での距離
      端点ではモーフィングがほぼ恒等写像になるので、そこでの距離は
      「合成器の実装差」だけを表す。中間 alpha の距離がこれと同程度なら、
      差は合成器由来でモーフィング由来ではない。

  [尺度] 隣接 alpha 間の距離
      MATLAB の WAV どうし（alpha=0.50 と 0.75 など）の距離。
      「本当に違う音」がどれくらいの数値になるかの物差し。
      測定値がこれより 1 桁以上小さければ、一致とみなしてよい。

--------------------------------------------------------------------------
時間長について
--------------------------------------------------------------------------
時間長だけは合成器や乱数の影響を受けない。時間軸モーフィングが
正しく移植できているかの、最も鋭い検査になる。
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import sys

import numpy as np


# ===========================================================================
# 特徴量
# ===========================================================================

def analyze(x: np.ndarray, fs: int, frame_period: float = 5.0):
    """WORLD で分析して F0 とスペクトル包絡を返す。"""
    import pyworld

    x = np.ascontiguousarray(x, dtype=np.float64)
    f0, t = pyworld.harvest(x, fs, frame_period=frame_period)
    sp = pyworld.cheaptrick(x, f0, t, fs)
    return f0, sp


def mel_cepstrum(sp: np.ndarray, fs: int, n_coef: int = 24,
                 n_mel: int = 40) -> np.ndarray:
    """スペクトル包絡からメルケプストラムを求める（MCD 用）。

    pysptk に依存せず、メルフィルタバンク + DCT で近似する。
    絶対値ではなく条件間の相対比較に使うので、この近似で足りる。
    """
    n_fbin = sp.shape[1]
    freq = np.arange(n_fbin) / n_fbin * fs / 2

    def hz2mel(f):
        return 2595.0 * np.log10(1.0 + f / 700.0)

    mel_edges = np.linspace(hz2mel(0), hz2mel(fs / 2), n_mel + 2)
    hz_edges = 700.0 * (10.0 ** (mel_edges / 2595.0) - 1.0)

    fb = np.zeros((n_mel, n_fbin))
    for m in range(n_mel):
        lo, ctr, hi = hz_edges[m], hz_edges[m + 1], hz_edges[m + 2]
        left = (freq >= lo) & (freq <= ctr)
        right = (freq > ctr) & (freq <= hi)
        fb[m, left] = (freq[left] - lo) / max(ctr - lo, 1e-9)
        fb[m, right] = (hi - freq[right]) / max(hi - ctr, 1e-9)
    # 行ごとに正規化しないと、帯域幅の広い高域フィルタだけ値が大きくなり
    # MCD が桁違いに膨らむ
    fb /= np.maximum(fb.sum(axis=1, keepdims=True), 1e-9)

    log_mel = np.log(np.maximum(sp @ fb.T, 1e-30))

    # 正規直交 DCT-II。正規化を省くと MCD が標準的なスケールから外れる
    k = np.arange(n_coef)[None, :]
    m = np.arange(n_mel)[:, None]
    dct = np.cos(np.pi * k * (2 * m + 1) / (2 * n_mel))
    dct *= np.sqrt(2.0 / n_mel)
    dct[:, 0] *= 1.0 / np.sqrt(2.0)
    return log_mel @ dct


def compare_features(xa: np.ndarray, xb: np.ndarray, fs: int) -> dict:
    """2 つの波形を再分析して距離を測る。"""
    f0a, spa = analyze(xa, fs)
    f0b, spb = analyze(xb, fs)

    n = min(len(f0a), len(f0b))
    f0a, f0b, spa, spb = f0a[:n], f0b[:n], spa[:n], spb[:n]

    # --- F0 (cent)。両方とも有声のフレームのみ ---
    m = (f0a > 0) & (f0b > 0)
    cent = (1200.0 * np.abs(np.log2(f0a[m] / f0b[m]))) if np.any(m) else np.array([0.0])

    # --- VUV 不一致率 ---
    vuv_err = float(np.mean((f0a > 0) != (f0b > 0)))

    # --- LSD (dB) ---
    d = 10 * np.log10(np.maximum(spa, 1e-30)) - 10 * np.log10(np.maximum(spb, 1e-30))
    lsd = float(np.sqrt(np.mean(d ** 2)))

    # --- MCD (dB)。0 次（パワー）は除くのが慣例 ---
    ca, cb = mel_cepstrum(spa, fs), mel_cepstrum(spb, fs)
    diff = ca[:, 1:] - cb[:, 1:]
    mcd = float(np.mean((10.0 / np.log(10)) * np.sqrt(2.0 * np.sum(diff ** 2, axis=1))))

    return {"f0_cent": float(np.mean(cent)), "f0_cent_max": float(np.max(cent)),
            "vuv_err": vuv_err, "lsd": lsd, "mcd": mcd, "n_frames": int(n)}


def roundtrip(x: np.ndarray, fs: int) -> np.ndarray:
    """分析して再合成する（対照条件 [床] の生成用）。"""
    import pyworld

    x = np.ascontiguousarray(x, dtype=np.float64)
    f0, t = pyworld.harvest(x, fs)
    sp = pyworld.cheaptrick(x, f0, t, fs)
    ap = pyworld.d4c(x, f0, t, fs)
    return pyworld.synthesize(f0, sp, ap, fs)


# ===========================================================================
# 本体
# ===========================================================================

def parse_tag(path: str) -> str | None:
    """ファイル名から alpha のタグを取り出す。"""
    m = re.search(r"(?:alpha|_a|α)[_-]?([01](?:\.\d+)?)", os.path.basename(path))
    return f"{float(m.group(1)):.6g}" if m else None


def nearest_tag(tag: str, cpp_paths: dict, tol: float = 0.01):
    """完全一致が無いとき、数値として最も近い alpha を探す。

    GUI 側のファイル名が 2 桁 (0.49) で、C++ 側が 4 桁 (0.4942) のように
    桁数が違うだけのことがあるので、その差を吸収する。
    """
    try:
        want = float(tag)
    except (TypeError, ValueError):
        return None
    best, best_d = None, None
    for t in cpp_paths:
        try:
            d = abs(float(t) - want)
        except ValueError:
            continue
        if best_d is None or d < best_d:
            best, best_d = t, d
    return best if best_d is not None and best_d <= tol else None


def load_wav(path: str):
    import soundfile as sf
    x, fs = sf.read(path, always_2d=False)
    if x.ndim > 1:
        x = x.mean(axis=1)   # ステレオならモノラル化
    return np.asarray(x, dtype=np.float64), int(fs)


def main() -> None:
    p = argparse.ArgumentParser(
        description="MATLAB GUI の WAV と C++ 版の WAV を比較する")
    p.add_argument("dir", help="C++ 側の WAV があるディレクトリ")
    p.add_argument("--matlab", required=True,
                   help="MATLAB 側 WAV のグロブ (例: 'matlab_wav/*.wav')")
    p.add_argument("--frame-period", type=float, default=0.005,
                   help="フレーム周期 [s]。時間長差が合成器の慣習か判定するのに使う")
    p.add_argument("--sweep", action="store_true",
                   help="alpha が不明なとき、全 alpha と突き合わせて最良を探す")
    args = p.parse_args()

    ml_paths = sorted(glob.glob(args.matlab))
    if not ml_paths:
        sys.exit(f"MATLAB 側 WAV が見つかりません: {args.matlab}")

    cpp_paths = {}
    for q in sorted(glob.glob(os.path.join(args.dir, "cpp_alpha*.wav"))):
        t = re.search(r"cpp_alpha(.+)\.wav$", os.path.basename(q)).group(1)
        cpp_paths[t] = q
    if not cpp_paths:
        sys.exit(f"{args.dir} に cpp_alpha*.wav がありません。"
                 f"先に synthesize_cpp.py を実行してください")

    print(f"MATLAB 側: {len(ml_paths)} ファイル / C++ 側: {len(cpp_paths)} 条件\n")

    # ---- 対照条件 [尺度]: MATLAB の WAV どうしの距離 ----
    scale = None
    if len(ml_paths) >= 2:
        xa, fs = load_wav(ml_paths[0])
        xb, _ = load_wav(ml_paths[1])
        r = compare_features(xa, xb, fs)
        scale = r
        print(f"[尺度] 隣接条件間の距離 "
              f"({os.path.basename(ml_paths[0])} vs {os.path.basename(ml_paths[1])})")
        print(f"       LSD {r['lsd']:.3f} dB / MCD {r['mcd']:.3f} dB / "
              f"F0 {r['f0_cent']:.2f} cent")
        print("       これが「本当に違う音」の目安。測定値はこれより十分小さいこと\n")

    results = []
    for mp in ml_paths:
        xm, fs_m = load_wav(mp)
        tag = parse_tag(mp)

        # ---- 対応する C++ の条件を決める ----
        near = None if tag is None else (tag if tag in cpp_paths
                                         else nearest_tag(tag, cpp_paths))
        if args.sweep or near is None:
            cands = list(cpp_paths.items())
            scored = []
            for t, q in cands:
                xc, _ = load_wav(q)
                scored.append((compare_features(xm, xc, fs_m)["mcd"], t, q))
            scored.sort()
            best_mcd, tag, cq = scored[0]
            note = (f"  （sweep: 最良は alpha={tag}, MCD {best_mcd:.3f} dB。"
                    f"次点 alpha={scored[1][1]} が {scored[1][0]:.3f} dB）"
                    if len(scored) > 1 else "")
        else:
            note = "" if near == tag else f"  （alpha={tag} に最も近い {near} と対応づけ）"
            tag = near
            cq = cpp_paths[tag]

        xc, fs_c = load_wav(cq)
        if fs_c != fs_m:
            print(f"!! {os.path.basename(mp)}: 標本化周波数が違います "
                  f"({fs_m} vs {fs_c})。スキップします")
            continue

        print(f"=== {os.path.basename(mp)}  vs  {os.path.basename(cq)} ==={note}")

        # ---- 時間長。合成器にも乱数にも影響されない、最も鋭い検査 ----
        dur_m, dur_c = len(xm) / fs_m, len(xc) / fs_c
        rel = abs(dur_m - dur_c) / dur_m
        # 差がちょうどフレーム周期の整数倍なら、合成器の末尾の扱いの違い。
        # pyworld は「フレーム数 x 周期」、MATLAB の Synthesis は
        # 最終フレーム時刻ちょうどで終わるため、1 フレーム分ずれる
        n_fp = abs(dur_m - dur_c) / args.frame_period
        boundary = abs(n_fp - round(n_fp)) < 0.05 and round(n_fp) <= 2
        if rel < 1e-3:
            verdict = "OK"
        elif boundary:
            verdict = f"OK（{round(n_fp)} フレーム分。合成器の末尾の扱いの違い）"
        else:
            verdict = "要確認"
        print(f"  時間長        : MATLAB {dur_m:.4f}s / C++ {dur_c:.4f}s  "
              f"(相対差 {rel:.2e})  [{verdict}]")
        if rel >= 1e-3 and not boundary:
            print("                  時間軸モーフィングの移植を疑ってください")

        r = compare_features(xm, xc, fs_m)
        print(f"  F0 誤差       : 平均 {r['f0_cent']:.2f} cent / "
              f"最大 {r['f0_cent_max']:.2f} cent")
        print(f"  VUV 不一致率  : {r['vuv_err'] * 100:.2f} %")
        print(f"  LSD           : {r['lsd']:.3f} dB")
        print(f"  MCD           : {r['mcd']:.3f} dB")

        # ---- 対照条件 [床]: 分析再合成の往復誤差 ----
        floor = compare_features(xc, roundtrip(xc, fs_c), fs_c)
        print(f"  [床] 往復誤差 : LSD {floor['lsd']:.3f} dB / MCD {floor['mcd']:.3f} dB")

        ratio = r["mcd"] / max(floor["mcd"], 1e-9)
        # 時間長と F0 は合成器の乱数に影響されないぶん MCD より鋭いので、
        # ここが外れていたら MCD 比によらず不一致と判断する
        if rel >= 1e-3 and not boundary:
            j = "NG（時間長が一致しません。時間軸モーフィングを疑ってください）"
        elif r["f0_cent"] > 10.0:
            j = "NG（F0 が一致しません。F0 モーフィングか時間写像を疑ってください）"
        elif ratio < 1.5:
            j = "OK（床と同程度。モーフィングの差は検出限界以下）"
        elif scale and r["mcd"] < scale["mcd"] / 10:
            j = "OK（尺度の 1/10 未満。実用上は一致）"
        elif ratio < 3.0:
            j = "要確認（床の数倍。合成器の実装差の範囲かもしれません）"
        else:
            j = "NG（床に対して大きすぎます）"
        print(f"  判定          : {j}  (測定値 / 床 = {ratio:.2f})\n")

        results.append((os.path.basename(mp), tag, r, floor))

    # ---- 対照条件 [較正]: 端点での距離 ----
    ends = [x for x in results if x[1] in ("0", "1")]
    if ends:
        print("[較正] 端点 (alpha=0 / 1) での距離")
        for name, tag, r, _ in ends:
            print(f"       alpha={tag}: LSD {r['lsd']:.3f} dB / MCD {r['mcd']:.3f} dB")
        print("       端点ではモーフィングがほぼ恒等写像なので、これは合成器の")
        print("       実装差だけを表す。中間 alpha がこれと同程度なら差は")
        print("       モーフィング由来ではない\n")

        mids = [x for x in results if x[1] not in ("0", "1")]
        if mids:
            e = np.mean([r["mcd"] for _, _, r, _ in ends])
            m = np.mean([r["mcd"] for _, _, r, _ in mids])
            print(f"       端点平均 MCD {e:.3f} dB / 中間平均 MCD {m:.3f} dB "
                  f"(比 {m / max(e, 1e-9):.2f})")
            if m / max(e, 1e-9) < 1.5:
                print("       -> 中間でも悪化していません。モーフィング部は一致とみなせます")
            else:
                print("       -> 中間で悪化しています。周波数軸または時間軸の")
                print("          モーフィングに差がある可能性があります")


if __name__ == "__main__":
    main()
