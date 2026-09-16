#!/usr/bin/env python3
"""
synthesize_cpp.py

test_matlab_parity が書き出した npy を WORLD で合成して WAV にする。

    python synthesize_cpp.py out/

--------------------------------------------------------------------------
npy の向きについて
--------------------------------------------------------------------------
C++ 側は (n_frame, n_fbin) の C 連続で書いており、これは pyworld が要求する
向きとそのまま一致する。転置は不要。

--------------------------------------------------------------------------
合成器の違いについて
--------------------------------------------------------------------------
MATLAB 版 GUI の合成器と pyworld (WORLD の C 実装) は別実装なので、
同じパラメータを与えても出力波形は一致しない。この差は compare_audio.py の
対照条件で定量化する。
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import sys

import numpy as np


def synthesize(dir_: str, tag: str, fs: float, frame_period: float) -> str:
    import pyworld
    import soundfile as sf

    def load(key):
        return np.ascontiguousarray(
            np.load(os.path.join(dir_, f"cpp_alpha{tag}_{key}.npy")), dtype=np.float64)

    f0 = load("f0")
    sp = load("spectrogram")     # (n_frame, n_fbin) — pyworld と同じ向き
    ap = load("aperiodicity")
    tp = load("temporal_positions")

    if sp.shape[0] != len(f0) or ap.shape != sp.shape:
        raise SystemExit(f"alpha={tag}: 形状が不整合です "
                         f"f0={f0.shape} sp={sp.shape} ap={ap.shape}")

    y = pyworld.synthesize(f0, sp, ap, int(fs), frame_period * 1000.0)

    # 末尾の長さを MATLAB の Synthesis に合わせる。
    # pyworld は「フレーム数 x フレーム周期」の長さを返すが、MATLAB 版は
    # 最終フレーム時刻ちょうどで終わるので、そのままだと常に 1 フレーム分
    # (5 ms) 長くなり、時間長の比較が必ず外れる
    n_want = int(round(tp[-1] * fs))
    if len(y) > n_want:
        y = y[:n_want]
    elif len(y) < n_want:
        y = np.pad(y, (0, n_want - len(y)))

    out = os.path.join(dir_, f"cpp_alpha{tag}.wav")
    sf.write(out, y, int(fs))
    return out


def main() -> None:
    p = argparse.ArgumentParser(description="C++ 版の出力を WAV に合成する")
    p.add_argument("dir", help="test_matlab_parity の出力ディレクトリ")
    p.add_argument("--fs", type=float, default=None,
                   help="標本化周波数（省略時は manifest.json から取得）")
    p.add_argument("--frame-period", type=float, default=0.005,
                   help="フレーム周期 [s]（既定 0.005）")
    args = p.parse_args()

    fs = args.fs
    if fs is None:
        import json
        man = os.path.join(args.dir, "manifest.json")
        if not os.path.exists(man):
            sys.exit("manifest.json が無いので --fs を指定してください")
        fs = json.load(open(man))["objects"][0]["sampling_frequency"]

    tags = sorted({re.search(r"cpp_alpha(.+)_f0\.npy$", os.path.basename(p)).group(1)
                   for p in glob.glob(os.path.join(args.dir, "cpp_alpha*_f0.npy"))})
    if not tags:
        sys.exit(f"{args.dir} に cpp_alpha*_f0.npy がありません")

    for t in tags:
        out = synthesize(args.dir, t, fs, args.frame_period)
        import soundfile as sf
        info = sf.info(out)
        print(f"alpha={t:8s} -> {out}  ({info.duration:.4f} s, {info.samplerate} Hz)")


if __name__ == "__main__":
    main()
