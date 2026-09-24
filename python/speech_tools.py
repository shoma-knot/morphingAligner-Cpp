"""morphingAligner から呼び出す音声解析ツール（フォルマント推定・音素セグメンテーション）。

使い方:
    python speech_tools.py <request.json> <response.json>

C++ 側は要求を JSON ファイルで渡し、結果を JSON ファイルで受け取る。コマンドライン
引数を一時ファイルのパスだけにしているのは、Windows でコマンドライン経由の日本語
（書き起こし・パス）が ANSI コードページで化けるのを避けるため。ファイルは UTF-8。

要求:
    {"command": "formants", "wav": "...", "max_formant_hz": 5500, "num_formants": 5,
     "num_tracks": 4, "time_step": 0.005}
    {"command": "align", "wav": "...", "text": "...", "acoustic_model": "japanese_mfa",
     "dictionary": "japanese_mfa"}

応答（成功時 ok=true。失敗時は ok=false と error）:
    formants: {"ok": true, "times": [...], "tracks": [[F1...], [F2...], ...]}
              未定義のフレームは null。
    align:    {"ok": true, "tiers": [{"name": "words", "intervals": [[start, end, label], ...]},
                                     {"name": "phones", ...}]}

経過は標準出力/標準エラーに出す（C++ 側がログファイルに回し、失敗時に末尾を表示する）。
"""

import json
import math
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import traceback


class ToolError(Exception):
    """想定内の失敗（入力の不備や MFA の失敗）。メッセージだけを C++ 側に返す。"""


def output_tail(text, n=3):
    """子プロセス出力の末尾 n 行（空行を除く）。失敗理由として error に添える。"""
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    return "\n".join(lines[-n:])


def conda_env():
    """この Python が属する conda 環境の実行ファイルと DLL を PATH に通した環境変数を返す。

    micromamba 環境を activate せずに python.exe を直接起動すると、Library/bin が PATH に
    無いため MFA が libsndfile.dll などを読めずに落ちる。子プロセスにはこれを渡す。
    """
    prefix = pathlib.Path(sys.prefix)
    if os.name == "nt":
        dirs = [
            prefix,
            prefix / "Library" / "mingw-w64" / "bin",
            prefix / "Library" / "usr" / "bin",
            prefix / "Library" / "bin",
            prefix / "Scripts",
        ]
    else:
        dirs = [prefix / "bin"]
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join([str(d) for d in dirs] + [env.get("PATH", "")])
    # 子プロセスの標準出力を UTF-8 に揃える（ログに日本語が混ざるため）。
    env["PYTHONIOENCODING"] = "utf-8"
    env["PYTHONUTF8"] = "1"
    return env


def load_mono(path):
    """音声を parselmouth で読み、モノラルにして返す。"""
    import parselmouth

    snd = parselmouth.Sound(str(path))
    if snd.n_channels > 1:
        snd = snd.convert_to_mono()
    return snd


def cmd_formants(req):
    """Burg 法（Praat の To Formant (burg)）でフォルマントを推定する。"""
    snd = load_mono(req["wav"])
    fm = snd.to_formant_burg(
        time_step=float(req.get("time_step", 0.005)),
        max_number_of_formants=float(req.get("num_formants", 5)),
        maximum_formant=float(req.get("max_formant_hz", 5500.0)),
        window_length=float(req.get("window_length", 0.025)),
        pre_emphasis_from=50.0,
    )
    times = [float(t) for t in fm.ts()]
    n_tracks = int(req.get("num_tracks", 4))
    tracks = []
    for k in range(1, n_tracks + 1):
        values = []
        for t in times:
            v = fm.get_value_at_time(k, t)
            values.append(None if v is None or math.isnan(v) else float(v))
        tracks.append(values)
    print(f"formants: {len(times)} frames, {n_tracks} tracks", flush=True)
    return {"ok": True, "times": times, "tracks": tracks}


def find_mfa(env):
    exe = shutil.which("mfa", path=env["PATH"])
    if exe is None:
        raise ToolError("mfa コマンドが見つかりません（montreal-forced-aligner が環境に入っていません）")
    return exe


def mfa_root():
    """MFA の作業ルート（学習済みモデルの置き場）。MFA と同じく MFA_ROOT_DIR を優先する。"""
    root = os.environ.get("MFA_ROOT_DIR")
    return pathlib.Path(root) if root else pathlib.Path.home() / "Documents" / "MFA"


def resolve_dictionary(name):
    """辞書の指定（ファイルパス、または mfa model download で取得した名前）を実ファイルにする。"""
    p = pathlib.Path(name)
    if p.is_file():
        return p
    p = mfa_root() / "pretrained_models" / "dictionary" / f"{name}.dict"
    if p.is_file():
        return p
    raise ToolError(f"発音辞書が見つかりません: {name}"
                       f"（mfa model download dictionary {name} で取得してください）")


def resolve_acoustic_model(name):
    """音響モデルの指定（zip のパス、またはダウンロード済みの名前）を zip ファイルにする。"""
    p = pathlib.Path(name)
    if p.is_file():
        return p
    p = mfa_root() / "pretrained_models" / "acoustic" / f"{name}.zip"
    if p.is_file():
        return p
    raise ToolError(f"音響モデルが見つかりません: {name}"
                    f"（mfa model download acoustic {name} で取得してください）")


def filter_dictionary(src, text, dst):
    """書き起こしに部分文字列として現れる語だけを残した辞書を dst に書く。

    japanese_mfa の辞書は 54 万行あり、MFA は実行のたびにこれを全部読み込むため
    1 発話でも数分かかる。MFA の単語分割（sudachi）が出す語は書き起こしの部分文字列
    なので、それ以外の語を落としても結果は変わらない（手元で数分 → 約 20 秒）。
    """
    keys = {text, text.lower()}
    n = 0
    with open(src, encoding="utf-8") as f, open(dst, "w", encoding="utf-8") as o:
        for line in f:
            word = line.split("\t", 1)[0].split(" ", 1)[0]
            if word and any(word in k for k in keys):
                o.write(line)
                n += 1
    if n == 0:
        raise ToolError("書き起こしの語が発音辞書に1つもありません")
    print(f"dictionary: {n} entries kept", flush=True)


def read_textgrid(path):
    """MFA が書き出した TextGrid を読み、区間ティアを [start, end, label] の列にする。"""
    from praatio import textgrid

    tg = textgrid.openTextgrid(str(path), includeEmptyIntervals=True)
    tiers = []
    for name in tg.tierNames:
        tier = tg.getTier(name)
        intervals = [[float(s), float(e), str(label)] for (s, e, label) in tier.entries]
        tiers.append({"name": name, "intervals": intervals})
    return tiers


def cmd_align(req):
    """Montreal Forced Aligner（mfa align_one）で単語・音素の境界を求める。"""
    text = str(req.get("text", "")).strip()
    if not text:
        raise ToolError("書き起こしテキストが空です")
    acoustic = resolve_acoustic_model(req.get("acoustic_model") or "japanese_mfa")
    dictionary = resolve_dictionary(req.get("dictionary") or "japanese_mfa")

    env = conda_env()
    mfa = find_mfa(env)

    # 作業ディレクトリは C++ 側が用意した場所（work_dir）の中に作る。アプリの終了で
    # このプロセスが強制終了されても、C++ 側の後始末でまとめて消えるようにするため。
    work = pathlib.Path(tempfile.mkdtemp(prefix="mfa_", dir=req.get("work_dir")))
    # MFA は実行のたびに音響モデルを <MFA_ROOT_DIR>/extracted_models へ展開し直す。既定の
    # ~/Documents/MFA のままだと、同時に走った MFA（base/target の同時実行や別の MFA）と
    # 展開が衝突して壊れた状態が残るので、呼び出しごとの作業ディレクトリに向ける。
    # モデルと辞書は上で実ファイルに解決済みなので、ルートを差し替えても見つかる。
    env["MFA_ROOT_DIR"] = str(work / "mfa_root")
    try:
        # 音声は parselmouth で読み直して 16bit WAV にする（mp3 などもこれで受けられる）。
        wav = work / "utt.wav"
        load_mono(req["wav"]).save(str(wav), "WAV")
        lab = work / "utt.lab"
        lab.write_text(text, encoding="utf-8")
        small_dict = work / "dictionary.dict"
        filter_dictionary(dictionary, text, small_dict)

        tg = work / "utt.TextGrid"
        cmd = [
            mfa, "align_one", str(wav), str(lab), str(small_dict), str(acoustic), str(tg),
            "--temporary_directory", str(work / "mfa_root"),
            "--output_format", "long_textgrid",
        ]
        print("run:", " ".join(cmd), flush=True)
        proc = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              encoding="utf-8", errors="replace")
        print(proc.stdout, flush=True)
        if proc.returncode != 0:
            raise ToolError(f"mfa align_one が失敗しました（終了コード {proc.returncode}）\n"
                            + output_tail(proc.stdout))
        if not tg.exists():
            raise ToolError("アラインメント結果（TextGrid）が出力されませんでした。"
                               "書き起こしが音声と合っていない可能性があります")
        tiers = read_textgrid(tg)
        print("align: " + ", ".join(f"{t['name']}={len(t['intervals'])}" for t in tiers), flush=True)
        return {"ok": True, "tiers": tiers}
    finally:
        shutil.rmtree(work, ignore_errors=True)


COMMANDS = {"formants": cmd_formants, "align": cmd_align}


def main():
    # ログはファイルへリダイレクトされる。Windows では既定が cp932 になり、IPA の
    # 音素記号を print した時点で落ちるので UTF-8 に固定する。
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    req_path, res_path = sys.argv[1], sys.argv[2]
    try:
        with open(req_path, encoding="utf-8") as f:
            req = json.load(f)
        command = req.get("command")
        if command not in COMMANDS:
            raise ToolError(f"未知のコマンド: {command}")
        res = COMMANDS[command](req)
    except ToolError as e:  # 想定内の失敗はメッセージだけ返す
        res = {"ok": False, "error": str(e).strip()}
    except Exception as e:  # 想定外の失敗はトレースバックをログに残す
        traceback.print_exc()
        res = {"ok": False, "error": f"{type(e).__name__}: {str(e).strip()}"}
    with open(res_path, "w", encoding="utf-8") as f:
        json.dump(res, f, ensure_ascii=False)
    return 0 if res.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
