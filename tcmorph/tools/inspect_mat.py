#!/usr/bin/env python3
"""
inspect_mat.py

.mat ファイルの中身の構造をツリー表示する。mat2json.py が読めない場合に、
実際のフィールド構成を調べるために使う。

    python3 inspect_mat.py morphdBase.mat
    python3 inspect_mat.py morphdBase.mat --depth 8 --items 5
    python3 inspect_mat.py morphdBase.mat --values      # 小さい配列は中身も出す

v7.3 (HDF5) と v7 以前の両方に対応。形式は自動判別する。

--------------------------------------------------------------------------
表示の読み方
--------------------------------------------------------------------------
    morphdBase : struct
      morphStr : struct[1x2]          <- 構造体配列。要素が 2 個
        [0]
          worldParameter : struct
            samplingFrequency : double (1,1) = 44100
            span : double (44100,1)
          timeAnchor : double (1,3) = [0.2, 0.5, 0.75]
        [1]
          ...

  * struct        構造体（フィールドを持つ）
  * struct[RxC]   構造体配列
  * cell[RxC]     セル配列
  * char          文字列
  * double (R,C)  数値配列。括弧内は MATLAB での形

--------------------------------------------------------------------------
出力の貼り付けについて
--------------------------------------------------------------------------
既定では要素数や深さを制限して短くしてある。構造を人に見せる用途なら
そのまま貼れる長さになるはず。全部見たいときは --full を付ける。
"""

from __future__ import annotations

import argparse
import sys

import numpy as np


# ===========================================================================
# 共通の表示ヘルパ
# ===========================================================================

def fmt_shape(shape) -> str:
    return "(" + ",".join(str(int(s)) for s in shape) + ")"


def fmt_values(a: np.ndarray, limit: int = 8) -> str:
    """小さい数値配列なら中身を文字列にする。大きければ空文字。"""
    flat = np.asarray(a).ravel()
    if flat.size == 0:
        return " = []"
    if flat.size > limit:
        # 大きい配列は代表値だけ出す。値域が分かると単位の推測に役立つ
        try:
            f = flat.astype(float)
            return f" = [min {f.min():.6g}, max {f.max():.6g}, ...{flat.size} 要素]"
        except (ValueError, TypeError):
            return ""
    try:
        return " = [" + ", ".join(f"{float(v):.6g}" for v in flat) + "]"
    except (ValueError, TypeError):
        return ""


class Printer:
    def __init__(self, max_depth: int, max_items: int, show_values: bool):
        self.max_depth = max_depth
        self.max_items = max_items
        self.show_values = show_values
        self.lines: list[str] = []

    def add(self, depth: int, text: str) -> None:
        self.lines.append("  " * depth + text)

    def dump(self) -> None:
        print("\n".join(self.lines))


# ===========================================================================
# v7 以前 (scipy)
# ===========================================================================

def walk_scipy(node, name: str, depth: int, p: Printer) -> None:
    import scipy.io as sio

    if depth > p.max_depth:
        p.add(depth, f"{name} : ... (深さ制限)")
        return

    # --- 構造体（スカラー） ---
    if isinstance(node, sio.matlab.mat_struct):
        # 直前に "[0]" のような見出しを出している場合は名前が空になる。
        # そこで ": struct" を重ねると冗長なので、フィールドだけを続ける
        if name:
            p.add(depth, f"{name} : struct")
            depth += 1
        for f in node._fieldnames:
            walk_scipy(getattr(node, f), f, depth, p)
        return

    if isinstance(node, np.ndarray):
        # --- 構造体配列 / セル配列 ---
        if node.dtype == object:
            elems = node.ravel()
            is_struct = elems.size > 0 and isinstance(elems[0], sio.matlab.mat_struct)

            # MATLAB では 1x1 の構造体配列はスカラー構造体と同じ意味なので、
            # 余計な階層を作らずそのまま畳む
            if is_struct and elems.size == 1:
                walk_scipy(elems[0], name, depth, p)
                return

            kind = "struct" if is_struct else "cell"
            p.add(depth, f"{name} : {kind}{fmt_shape(node.shape)}")
            n = min(elems.size, p.max_items)
            for i in range(n):
                p.add(depth + 1, f"[{i}]")
                walk_scipy(elems[i], "", depth + 2, p)
            if elems.size > n:
                p.add(depth + 1, f"... 他 {elems.size - n} 要素（同じ構造とみなして省略）")
            return

        # --- 文字列 ---
        if node.dtype.kind in "US":
            s = str(node.ravel()[0]) if node.size else ""
            p.add(depth, f"{name} : char = '{s[:60]}'")
            return

        # --- 数値配列 ---
        v = fmt_values(node) if p.show_values or node.size <= 8 else ""
        p.add(depth, f"{name} : {node.dtype} {fmt_shape(node.shape)}{v}")
        return

    # --- スカラー ---
    p.add(depth, f"{name} : {type(node).__name__} = {node}")


def inspect_scipy(path: str, p: Printer) -> None:
    import scipy.io as sio

    raw = sio.loadmat(path, struct_as_record=False, squeeze_me=False)
    names = [k for k in raw if not k.startswith("__")]
    p.add(0, f"トップレベル変数: {names}")
    p.add(0, "")
    for k in names:
        walk_scipy(raw[k], k, 0, p)


# ===========================================================================
# v7.3 (HDF5)
# ===========================================================================

def walk_h5(f, node, name: str, depth: int, p: Printer, seen: set) -> None:
    import h5py

    if depth > p.max_depth:
        p.add(depth, f"{name} : ... (深さ制限)")
        return

    cls = node.attrs.get("MATLAB_class", b"")
    cls = cls.decode() if isinstance(cls, bytes) else str(cls)

    # --- Group = 構造体 or 構造体配列 ---
    if isinstance(node, h5py.Group):
        # フィールドが持つ参照の個数で、構造体配列かどうかを判断する
        n_elem = 1
        for k in node.keys():
            v = node[k]
            if isinstance(v, h5py.Dataset) and v.dtype == h5py.ref_dtype:
                n_elem = max(n_elem, int(np.array(v).size))

        label = "struct" if n_elem == 1 else f"struct[{n_elem}]"
        p.add(depth, f"{name} : {label}" + (f"  (MATLAB_class={cls})" if cls and cls != "struct" else ""))

        if n_elem == 1:
            for k in node.keys():
                walk_h5(f, node[k], k, depth + 1, p, seen)
            return

        for i in range(min(n_elem, p.max_items)):
            p.add(depth + 1, f"[{i}]")
            for k in node.keys():
                v = node[k]
                if isinstance(v, h5py.Dataset) and v.dtype == h5py.ref_dtype:
                    refs = np.array(v).ravel()
                    walk_h5(f, f[refs[i]], k, depth + 2, p, seen)
                else:
                    walk_h5(f, v, k, depth + 2, p, seen)
        if n_elem > p.max_items:
            p.add(depth + 1, f"... 他 {n_elem - p.max_items} 要素（同じ構造とみなして省略）")
        return

    # --- Dataset ---
    if isinstance(node, h5py.Dataset):
        # 参照配列 = ネストした構造体 / セル
        if node.dtype == h5py.ref_dtype:
            refs = np.array(node).ravel()
            if refs.size == 1:
                walk_h5(f, f[refs[0]], name, depth, p, seen)
                return
            kind = "cell" if cls == "cell" else "struct"
            p.add(depth, f"{name} : {kind}[{refs.size}]")
            for i in range(min(refs.size, p.max_items)):
                p.add(depth + 1, f"[{i}]")
                walk_h5(f, f[refs[i]], "", depth + 2, p, seen)
            if refs.size > p.max_items:
                p.add(depth + 1, f"... 他 {refs.size - p.max_items} 要素")
            return

        # 文字列
        if cls == "char":
            try:
                s = "".join(chr(c) for c in np.array(node).ravel() if c)
            except (ValueError, TypeError):
                s = "<復元できず>"
            p.add(depth, f"{name} : char = '{s[:60]}'")
            return

        # 数値配列。HDF5 は行優先なので MATLAB での形は逆順になる
        a = np.array(node)
        ml_shape = tuple(reversed(a.shape))
        v = fmt_values(a) if p.show_values or a.size <= 8 else ""
        p.add(depth, f"{name} : {a.dtype} {fmt_shape(ml_shape)}{v}")
        return

    p.add(depth, f"{name} : <未知のノード {type(node)}>")


def inspect_h5(path: str, p: Printer) -> None:
    import h5py

    with h5py.File(path, "r") as f:
        names = [k for k in f.keys() if not k.startswith("#")]
        p.add(0, f"トップレベル変数: {names}")
        if "#refs#" in f:
            p.add(0, "(#refs# あり = ネストした構造体またはセル配列を含む)")
        p.add(0, "")
        for k in names:
            walk_h5(f, f[k], k, 0, p, set())


# ===========================================================================

def is_hdf5(path: str) -> bool:
    with open(path, "rb") as f:
        return f.read(8) == b"\x89HDF\r\n\x1a\n"


def main() -> None:
    ap = argparse.ArgumentParser(description=".mat の構造をツリー表示する")
    ap.add_argument("mat")
    ap.add_argument("--depth", type=int, default=8, help="表示する最大の深さ")
    ap.add_argument("--items", type=int, default=2,
                    help="配列要素を何個まで展開するか（既定 2）")
    ap.add_argument("--values", action="store_true",
                    help="数値配列の中身（代表値）も表示する")
    ap.add_argument("--full", action="store_true",
                    help="制限なしで全部表示する")
    args = ap.parse_args()

    if args.full:
        args.depth, args.items, args.values = 99, 99, True

    fmt = "v7.3 / HDF5" if is_hdf5(args.mat) else "v7 以前"
    print(f"# {args.mat}  ({fmt})")
    print()

    p = Printer(args.depth, args.items, args.values)
    try:
        if is_hdf5(args.mat):
            inspect_h5(args.mat, p)
        else:
            inspect_scipy(args.mat, p)
    except Exception as e:
        print(f"読み取りに失敗しました: {type(e).__name__}: {e}", file=sys.stderr)
        sys.exit(1)

    p.dump()

    print()
    print("# この出力をそのまま貼り付けてください。")
    print("# 省略された部分が気になる場合は --full を付けて実行してください。")


if __name__ == "__main__":
    main()
