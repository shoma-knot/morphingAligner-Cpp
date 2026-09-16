#!/usr/bin/env python3
"""
mat2json.py

MATLAB 版 worldGUItools 系 GUI が save したモーフィング用構造体 (.mat) を、
C++ 版が読める形式に変換する。

MATLAB 版と C++ 版で「同じ MorphObj から同じ音声が出るか」を確認するのが目的
なので、アンカーだけでなく WORLD パラメータも MATLAB 側の値をそのまま運ぶ。
C++ 側で分析をやり直すと、WORLD 実装の差なのかモーフィング実装の差なのかが
切り分けられなくなるため。

--------------------------------------------------------------------------
対応している .mat のレイアウト
--------------------------------------------------------------------------
GUI の版によって構造が違うので、2 種類を自動判別する。
不明な場合は inspect_mat.py で構造を確認すること。

  reftgt : 2 素材固定。morphingStr を持つ edit.mat がこれ
      <var>
        worldPRef / worldPTgt                  WORLD パラメータ（参照 / 目標）
        timeAnchorReference / ...Target        (n_tanchor, 1)
        timeFreqAnchorReference / ...Target    (n_fanchor, n_tanchor)
        anchStr.tAnchRef / tAnchTgt / tfAnchor 予備の置き場所
        testMrate                              GUI で最後に使ったモーフィング率

  morphstr : N 素材。generalizedTCmorphing.m が直接受け取る形
      <var>.morphStr(ii).worldParameter / timeAnchor / timeFreqAnchor

reftgt では参照を素材 0、目標を素材 1 に割り当てる。したがって重み
alpha=0 が参照、alpha=1 が目標に対応し、GUI のモーフィング率と一致する。

--------------------------------------------------------------------------
型について
--------------------------------------------------------------------------
GUI によっては samplingFrequency が uint16、vuv が uint8、span が uint16 など
整数型で保存されている。すべて float64 に揃えてから書き出す。
span は長さだけを使うので、中身が波形でも標本番号でも影響しない。

--------------------------------------------------------------------------
出力されるファイル
--------------------------------------------------------------------------
  <出力先>/
      anchors.json          アンカーのみ（C++ 版 tc_morphing_anchor_io.hpp 用）
      manifest.json         WORLD パラメータのメタ情報と npy への参照
      world/<name>_*.npy    WORLD パラメータ本体

--------------------------------------------------------------------------
npy の向きについて（重要）
--------------------------------------------------------------------------
スペクトログラムと非周期性は shape (n_frame, n_fbin) の C 連続で保存する。
C++ 側は (n_fbin, n_frame) の列優先なので、メモリ上の並びが完全に一致し、
転置もループも無しに Eigen::Map で読める。

--------------------------------------------------------------------------
使い方
--------------------------------------------------------------------------
    python3 mat2json.py edit.mat out/
    python3 mat2json.py edit.mat out/ --names fhi mya
    python3 mat2json.py morphdBase.mat out/ --layout morphstr --var morphdBase
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Any

import numpy as np


# ===========================================================================
# .mat の読み込み（v7.3 = HDF5 と v7 以前で経路が分かれる）
# ===========================================================================

def _is_hdf5(path: str) -> bool:
    """MATLAB v7.3 は HDF5 なので、マジックナンバーで判別する。"""
    with open(path, "rb") as f:
        return f.read(8) == b"\x89HDF\r\n\x1a\n"


def _scipy_to_py(obj: Any) -> Any:
    """scipy の mat_struct を辞書・リスト・ndarray に正規化する。"""
    import scipy.io as sio

    if isinstance(obj, sio.matlab.mat_struct):
        return {name: _scipy_to_py(getattr(obj, name)) for name in obj._fieldnames}
    if isinstance(obj, np.ndarray) and obj.dtype == object:
        vals = [_scipy_to_py(v) for v in obj.ravel()]
        # MATLAB では 1x1 の構造体配列はスカラー構造体と同じ意味
        return vals[0] if len(vals) == 1 and isinstance(vals[0], dict) else vals
    return obj


def _load_scipy(path: str) -> dict:
    import scipy.io as sio
    raw = sio.loadmat(path, struct_as_record=False, squeeze_me=True)
    return {k: _scipy_to_py(v) for k, v in raw.items() if not k.startswith("__")}


def _h5_to_py(f, node) -> Any:
    """HDF5 のノードを辞書・リスト・ndarray に正規化する。

    MATLAB v7.3 では
      * スカラー構造体   -> フィールド名をキーに持つ Group（各値は 1 要素の参照配列）
      * 構造体配列 (1xN) -> 同じく Group だが、各フィールドが N 要素の参照配列
      * 数値配列         -> Dataset。HDF5 は行優先なので MATLAB の (r,c) は (c,r) に見える
    """
    import h5py

    if isinstance(node, h5py.Dataset):
        if node.dtype == h5py.ref_dtype:
            refs = np.array(node).ravel()
            vals = [_h5_to_py(f, f[r]) for r in refs]
            return vals[0] if len(vals) == 1 else vals
        arr = np.array(node)
        return arr.T if arr.ndim >= 2 else arr

    if isinstance(node, h5py.Group):
        fields = {k: node[k] for k in node.keys()}
        n = 1
        for v in fields.values():
            if isinstance(v, h5py.Dataset) and v.dtype == h5py.ref_dtype:
                n = max(n, int(np.array(v).size))
        if n == 1:
            return {k: _h5_to_py(f, v) for k, v in fields.items()}

        out = []
        for i in range(n):
            entry = {}
            for k, v in fields.items():
                if isinstance(v, h5py.Dataset) and v.dtype == h5py.ref_dtype:
                    refs = np.array(v).ravel()
                    entry[k] = _h5_to_py(f, f[refs[i]])
                else:
                    entry[k] = _h5_to_py(f, v)
            out.append(entry)
        return out

    raise TypeError(f"未知の HDF5 ノード型: {type(node)}")


def _load_h5(path: str) -> dict:
    import h5py
    with h5py.File(path, "r") as f:
        return {k: _h5_to_py(f, f[k]) for k in f.keys() if not k.startswith("#")}


def load_mat(path: str) -> dict:
    """形式を自動判別して .mat を読む。"""
    return _load_h5(path) if _is_hdf5(path) else _load_scipy(path)


# ===========================================================================
# 取り出しの補助
# ===========================================================================

def _get(d: Any, *names: str, required: bool = True):
    """フィールド名の表記ゆれを吸収して取り出す。"""
    for n in names:
        if isinstance(d, dict) and n in d:
            return d[n]
    if not required:
        return None
    have = sorted(d.keys()) if isinstance(d, dict) else type(d).__name__
    raise SystemExit(f"フィールド {names} が見つかりません。存在するのは: {have}")


def _scalar(x) -> float:
    """uint16 などで入っていることがあるので float に揃える。"""
    return float(np.asarray(x).ravel()[0])


def _vec(x) -> np.ndarray:
    """uint8 の vuv などもあるので float64 に揃えて 1 次元にする。"""
    return np.asarray(x, dtype=np.float64).ravel()


def _mat2d(x, n_frame: int, what: str) -> np.ndarray:
    """(n_fbin, n_frame) の向きに揃える。"""
    a = np.asarray(x, dtype=np.float64)
    if a.ndim != 2:
        raise SystemExit(f"{what}: 2 次元配列を期待しましたが shape={a.shape} でした")
    if a.shape[1] == n_frame:
        return a
    if a.shape[0] == n_frame:
        return a.T
    raise SystemExit(f"{what}: shape={a.shape} のどちらの軸もフレーム数 {n_frame} "
                     f"と一致しません")


# ===========================================================================
# レイアウトの判別と、素材の取り出し
# ===========================================================================

# 表記ゆれの候補。左から順に試す
REF_WORLD = ("worldPRef", "worldParameterRef", "worldPReference", "refWorldParameter")
TGT_WORLD = ("worldPTgt", "worldParameterTgt", "worldPTarget", "tgtWorldParameter")


def _is_reftgt(v: Any) -> bool:
    return isinstance(v, dict) and any(n in v for n in REF_WORLD)


def _pick_var(data: dict, var: str | None, layout: str) -> tuple[str, dict]:
    """対象の変数を決める。"""
    if var is not None:
        if var not in data:
            raise SystemExit(f"変数 '{var}' がありません。"
                             f"含まれる変数: {sorted(data.keys())}")
        return var, data[var]

    for k, v in data.items():
        if layout in ("auto", "reftgt") and _is_reftgt(v):
            return k, v
        if layout in ("auto", "morphstr") and isinstance(v, dict) and "morphStr" in v:
            return k, v

    raise SystemExit(f"対象の変数が見つかりません。--var で指定するか、"
                     f"inspect_mat.py で構造を確認してください。"
                     f"含まれる変数: {sorted(data.keys())}")


def _pick_anchor(root: dict, time_names: tuple, freq_names: tuple,
                 anch_time_names: tuple, anch_freq_names: tuple, tag: str):
    """時間アンカーと周波数アンカーを取り出す。

    GUI はトップレベルと anchStr の両方にアンカーらしきものを持っていることが
    あり、anchStr 側は別の格子に張り直した作業用データであることがある
    （たとえば時間アンカーが 24 本なのに (7,100) が入っている）。
    そこで「周波数アンカーの列数が時間アンカーの本数と一致するか」で
    正しい組み合わせを選ぶ。
    """
    anch = _get(root, "anchStr", required=False) or {}

    # --- 時間アンカー ---
    t = _get(root, *time_names, required=False)
    if t is None:
        t = _get(anch, *anch_time_names, required=False)
    if t is None:
        raise SystemExit(f"{tag}: 時間アンカー {time_names} / {anch_time_names} が "
                         f"見つかりません")
    t = _vec(t)
    n_tanchor = len(t)

    # --- 周波数アンカー。列数が n_tanchor に一致するものを採用する ---
    cands = []
    for nm in freq_names:
        v = _get(root, nm, required=False)
        if v is not None:
            cands.append((nm, v))
    for nm in anch_freq_names:
        v = _get(anch, nm, required=False)
        if v is not None:
            cands.append((f"anchStr.{nm}", v))

    chosen, rejected = None, []
    for nm, v in cands:
        a = np.atleast_2d(np.asarray(v, dtype=np.float64))
        if a.shape[1] == n_tanchor:
            chosen = (nm, a)
            break
        if a.shape[0] == n_tanchor:
            chosen = (nm, a.T)
            break
        rejected.append(f"{nm}{a.shape}")

    if chosen is None:
        raise SystemExit(f"{tag}: 時間アンカー {n_tanchor} 本に対応する周波数アンカーが "
                         f"見つかりません。候補と形: {rejected}")

    return t, chosen[1], chosen[0], rejected


def extract_objects(root: dict, layout: str, names: list[str] | None):
    """(name, world_dict, time_anchor, time_freq_anchor) のリストと付随情報を返す。"""
    extra: dict = {}

    # ---- reftgt レイアウト ----
    if layout == "reftgt" or (layout == "auto" and _is_reftgt(root)):
        extra["layout"] = "reftgt"
        w_ref = _get(root, *REF_WORLD)
        w_tgt = _get(root, *TGT_WORLD)

        t_ref, f_ref, src_ref, rej = _pick_anchor(
            root,
            ("timeAnchorReference", "timeAnchorRef"),
            ("timeFreqAnchorReference", "timeFreqAnchorRef"),
            ("tAnchRef", "referenceLocation"),
            ("tfAnchor", "ftAnchorRef"),
            "reference")
        t_tgt, f_tgt, src_tgt, rej2 = _pick_anchor(
            root,
            ("timeAnchorTarget", "timeAnchorTgt"),
            ("timeFreqAnchorTarget", "timeFreqAnchorTgt"),
            ("tAnchTgt",),
            ("ftAnchorTgt", "modTFAnchorTgt"),
            "target")

        print("  レイアウト: reftgt（参照 -> 素材 0、目標 -> 素材 1）")
        print(f"  アンカーの取得元: reference={src_ref}, target={src_tgt}")
        rejected = sorted(set(rej) | set(rej2))
        if rejected:
            print(f"  使わなかった候補（列数が時間アンカー数と合わないため、"
                  f"GUI の作業用データとみなしました）: {', '.join(rejected)}")

        # GUI が最後に使ったモーフィング率。比較時の alpha の手がかりになる
        for k in ("testMrate", "mRate", "morphingRate"):
            v = _get(root, k, required=False)
            if v is not None:
                extra["suggested_alpha"] = _scalar(v)
                print(f"  {k} = {extra['suggested_alpha']:.6f} "
                      f"（GUI で使われたモーフィング率。比較時の alpha の目安）")
                break

        v = _get(root, "vtl_ratio", required=False)
        if v is not None:
            extra["vtl_ratio"] = _scalar(v)
            if extra["vtl_ratio"] != 1.0:
                print(f"  ! vtl_ratio = {extra['vtl_ratio']} です。GUI 側で声道長の"
                      f"補正が別途かかっている可能性があり、その分は C++ 版では"
                      f"再現されません")

        for k in ("referenceFileName", "targetFileName"):
            v = _get(root, k, required=False)
            if isinstance(v, str):
                extra[k] = v

        nm = names or ["reference", "target"]
        return [(nm[0], w_ref, t_ref, f_ref), (nm[1], w_tgt, t_tgt, f_tgt)], extra

    # ---- morphstr レイアウト ----
    extra["layout"] = "morphstr"
    ms = _get(root, "morphStr")
    ms = ms if isinstance(ms, list) else [ms]
    print(f"  レイアウト: morphstr（素材 {len(ms)} 個）")

    out = []
    for ii, obj in enumerate(ms):
        name = names[ii] if names else f"object_{ii}"
        w = _get(obj, "worldParameter", "world_parameter")
        t = _vec(_get(obj, "timeAnchor", "time_anchor"))
        f = np.atleast_2d(np.asarray(
            _get(obj, "timeFreqAnchor", "time_freq_anchor"), dtype=np.float64))
        if f.shape[1] != len(t):
            if f.shape[0] == len(t):
                f = f.T
            else:
                raise SystemExit(f"素材 {name}: timeFreqAnchor の shape {f.shape} が "
                                 f"timeAnchor の長さ {len(t)} と合いません")
        out.append((name, w, t, f))
    return out, extra


# ===========================================================================
# 変換本体
# ===========================================================================

def convert(mat_path: str, out_dir: str, var: str | None, layout: str,
            names: list[str] | None) -> None:
    print(f"読み込み: {mat_path} "
          f"({'v7.3 / HDF5' if _is_hdf5(mat_path) else 'v7 以前'})")
    data = load_mat(mat_path)

    var_name, root = _pick_var(data, var, layout)
    print(f"  変数 '{var_name}' を使用")

    objs, extra = extract_objects(root, layout, names)
    if names and len(names) != len(objs):
        raise SystemExit(f"--names の個数 ({len(names)}) が素材数 ({len(objs)}) "
                         f"と一致しません")

    world_dir = os.path.join(out_dir, "world")
    os.makedirs(world_dir, exist_ok=True)

    anchors_json: dict = {"version": 1, "objects": []}
    manifest: dict = {"version": 1, "source": os.path.abspath(mat_path)}
    manifest.update(extra)
    manifest["objects"] = []
    warnings: list[str] = []

    for ii, (name, wp, time_anchor, tf) in enumerate(objs):
        # ---- WORLD パラメータ ----
        fs = _scalar(_get(wp, "samplingFrequency", "sampling_frequency", "fs"))

        # span が無い版もあるので signal で代替する。長さだけ使うので、
        # 中身が波形でも標本番号でも結果は変わらない
        span = _get(wp, "span", required=False)
        if span is None:
            span = _get(wp, "signal", required=False)
            if span is None:
                raise SystemExit(f"素材 {name}: span も signal もありません")
            warnings.append(f"{name}: span が無いので signal の長さを使いました")
        span_length = int(np.asarray(span).size)

        src = _get(wp, "source_parameter", "sourceParameter")
        spc = _get(wp, "spectrum_parameter", "spectrumParameter")

        src_t = _vec(_get(src, "temporal_positions", "temporalPositions"))
        spc_t = _vec(_get(spc, "temporal_positions", "temporalPositions"))
        f0 = _vec(_get(src, "f0", "fo"))
        vuv = _vec(_get(src, "vuv"))

        # wordTV2WmorphingEngineRev は source_parameter.f0 ではなく
        # worldParameter.f0_original を使う。無い版では f0 で代用する
        f0o = _get(wp, "f0_original", "f0Original", required=False)
        if f0o is None:
            f0o = _get(src, "f0_original", required=False)
        if f0o is None:
            f0_original = f0
            warnings.append(f"{name}: f0_original が無いので f0 で代用しました。"
                            f"morphingAligner 系の比較では差の原因になりえます")
        else:
            f0_original = _vec(f0o)
            if len(f0_original) != len(src_t):
                raise SystemExit(f"素材 {name}: f0_original の長さが "
                                 f"temporal_positions と一致しません")
        ap = _mat2d(_get(src, "aperiodicity"), len(src_t), f"{name}.aperiodicity")
        sp = _mat2d(_get(spc, "spectrogram"), len(spc_t), f"{name}.spectrogram")

        n_fbin = sp.shape[0]
        if ap.shape[0] != n_fbin:
            raise SystemExit(f"素材 {name}: 非周期性のビン数 {ap.shape[0]} が "
                             f"スペクトログラム {n_fbin} と一致しません")
        if len(f0) != len(src_t) or len(vuv) != len(src_t):
            raise SystemExit(f"素材 {name}: f0/vuv の長さが temporal_positions と "
                             f"一致しません")

        # npy は (n_frame, n_fbin) の C 連続で書く。
        # これは C++ 側の列優先 (n_fbin, n_frame) とメモリ配置が同一になる
        files: dict = {}

        def save(key: str, arr: np.ndarray) -> None:
            path = os.path.join(world_dir, f"{name}_{key}.npy")
            np.save(path, np.ascontiguousarray(arr, dtype=np.float64))
            files[key] = os.path.relpath(path, out_dir).replace(os.sep, "/")

        save("source_temporal_positions", src_t)
        save("spectrum_temporal_positions", spc_t)
        save("f0", f0)
        save("f0_original", f0_original)
        save("vuv", vuv)
        save("aperiodicity", ap.T)
        save("spectrogram", sp.T)

        entry: dict = {
            "name": name,
            "sampling_frequency": fs,
            "span_length": span_length,
            "n_source_frames": int(len(src_t)),
            "n_spectrum_frames": int(len(spc_t)),
            "n_fbin": int(n_fbin),
            "files": files,
        }
        for k in ("fileName", "fileFullPath"):
            v = _get(wp, k, required=False)
            if isinstance(v, str):
                entry[k] = v
        manifest["objects"].append(entry)

        # ---- アンカー。JSON 側は時刻ごとの行にして 0 詰めを落とす ----
        rows = []
        for jj in range(len(time_anchor)):
            col = tf[:, jj]
            rows.append([float(v) for v in col[col != 0.0]])

        anchors_json["objects"].append({
            "name": name,
            "time_anchor": [float(v) for v in time_anchor],
            "time_freq_anchor": rows,
        })

        # ---- C++ 側の検証条件を先に確認して、引っかかりそうなら警告 ----
        if not np.all(np.diff(time_anchor) > 0):
            bad = np.where(np.diff(time_anchor) <= 0)[0]
            warnings.append(f"{name}: timeAnchor が狭義単調増加ではありません "
                            f"(添字 {bad[:5].tolist()} 付近)")
        if time_anchor[0] <= 0:
            warnings.append(f"{name}: 先頭の timeAnchor が {time_anchor[0]} です。"
                            f"C++ 版は正の値を要求します")
        if time_anchor[-1] >= span_length / fs:
            warnings.append(f"{name}: 最終 timeAnchor {time_anchor[-1]:.4f}s が "
                            f"発話長 {span_length / fs:.4f}s 以上です")
        for jj, row in enumerate(rows):
            if len(row) > 1 and not np.all(np.diff(row) > 0):
                warnings.append(f"{name}: timeFreqAnchor 第 {jj} 列の周波数が "
                                f"狭義単調増加ではありません")
                break
        for jj, row in enumerate(rows):
            if any(v >= fs / 2 for v in row):
                warnings.append(f"{name}: timeFreqAnchor 第 {jj} 列に Nyquist "
                                f"{fs / 2:.0f}Hz 以上の値があります")
                break

        n_kinds = sorted(set(len(r) for r in rows))
        print(f"  [{ii}] {name}: fs={fs:.0f}Hz  duration={span_length / fs:.4f}s  "
              f"frames={len(spc_t)}  bins={n_fbin}  "
              f"anchors={len(time_anchor)}時刻x{n_kinds}本")

    # 全素材でアンカー本数が揃っているか（C++ 版が前提にしている条件）
    counts = [[len(r) for r in o["time_freq_anchor"]] for o in anchors_json["objects"]]
    if any(c != counts[0] for c in counts):
        warnings.append("素材間で周波数アンカー本数が揃っていません。"
                        "C++ 版は読み込み時に弾きます")
    if any(len(o["time_anchor"]) != len(anchors_json["objects"][0]["time_anchor"])
           for o in anchors_json["objects"]):
        warnings.append("素材間で時間アンカー数が揃っていません")

    with open(os.path.join(out_dir, "anchors.json"), "w") as f:
        json.dump(anchors_json, f, indent=2, ensure_ascii=False)
    with open(os.path.join(out_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)

    print(f"\n書き出し: {os.path.join(out_dir, 'anchors.json')}")
    print(f"          {os.path.join(out_dir, 'manifest.json')}")
    print(f"          {world_dir}/*.npy")

    if warnings:
        print("\n警告（C++ 版の読み込み時に弾かれる可能性があります）:")
        for w in warnings:
            print(f"  * {w}")
    else:
        print("\n検証: 問題は見つかりませんでした")

    # レイアウトに応じて次に叩くコマンドを案内する。
    # エンジンは .mat を作った GUI で決まるので、ここで取り違えないようにする
    if manifest.get("layout") == "reftgt":
        cmd, note = "./bin/morph_aligner", "morphingAligner 系（wordTV2WmorphingEngineRev）"
    else:
        cmd, note = "./bin/morph_generalized", "morphContinuumGen 系（generalizedTCmorphing）"
    print(f"\n次の手順（{note}）:")
    if "suggested_alpha" in manifest:
        a = manifest["suggested_alpha"]
        print(f"  {cmd} {out_dir} {a:.6g} 0 0.5 1")
        print(f"  （{a:.6g} は GUI で使われたモーフィング率）")
    else:
        print(f"  {cmd} {out_dir} 0 0.5 1")
    print(f"  python3 tools/synthesize_cpp.py {out_dir}")


def main() -> None:
    p = argparse.ArgumentParser(
        description="MATLAB の MorphObj (.mat) を C++ 版用の JSON + npy に変換する")
    p.add_argument("mat", help="入力 .mat ファイル")
    p.add_argument("out", help="出力ディレクトリ")
    p.add_argument("--var", default=None, help="対象の変数名（省略時は自動探索）")
    p.add_argument("--layout", default="auto", choices=["auto", "reftgt", "morphstr"],
                   help="構造のレイアウト（既定 auto）")
    p.add_argument("--names", nargs="*", default=None,
                   help="素材の名前（reftgt の既定は reference, target）")
    args = p.parse_args()

    if not os.path.exists(args.mat):
        sys.exit(f"ファイルがありません: {args.mat}")
    convert(args.mat, args.out, args.var, args.layout, args.names)


if __name__ == "__main__":
    main()
