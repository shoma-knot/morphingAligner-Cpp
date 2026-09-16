# tcmorph

Hideki Kawahara 氏の [worldGUItools](https://github.com/HidekiKawahara/worldGUItools) に含まれる
音声モーフィングエンジンを C++17 + Eigen に移植したヘッダオンリーライブラリ。

MATLAB 版が持つ 2 つのエンジンをどちらも移植してある。

| エンジン | 素材数 | MATLAB 側の呼び出し元 |
|---|---|---|
| `GeneralizedTCMorphing` | N 個 | `morphContinuumGen`, `threeItemMorpherGeneralUI` |
| `aligner::WordTV2WMorphing` | 2 個（参照 / 目標） | `morphingAligner`, `morphingSoundGenerator` |

どちらも WORLD の分析パラメータを入力に取り、モーフィング済みのパラメータを返す。
合成は行わないので、WORLD の合成器（pyworld でも MATLAB の `Synthesis` でも）に渡す。

付属の Python ツールで MATLAB が保存した `.mat` を変換でき、**MATLAB 環境は不要**。

---

## 依存

| | 用途 | 導入 |
|---|---|---|
| Eigen 3.4 | 行列演算 | `apt install libeigen3-dev` |
| nlohmann/json | I/O ヘッダのみ | `apt install nlohmann-json3-dev` |
| OpenMP | 任意（並列化） | gcc に同梱 |
| numpy, scipy, h5py | `.mat` の読み取り | `pip install numpy scipy h5py` |
| pyworld, soundfile | 合成と比較 | `pip install pyworld soundfile` |

エンジンのヘッダだけを使うなら Eigen 以外は不要。

    make check      # 依存の確認
    make            # examples を bin/ にビルド
    make install    # ヘッダを /usr/local/include/tcmorph へ

---

## 構成

    include/tcmorph/
      tcmorph.hpp                  一括インクルード
      generalized_tc_morphing.hpp  generalizedTCmorphing.m の移植
      word_tv2w_morphing.hpp       wordTV2WmorphingEngineRev.m の移植
      anchor_io.hpp                アンカーの JSON 読み書き
      world_io.hpp                 WORLD パラメータの npy 読み書き
    examples/
      morph_aligner.cpp            2 素材エンジンの実行例
      morph_generalized.cpp        N 素材エンジンの実行例
    tools/
      inspect_mat.py               .mat の構造を調べる
      mat2json.py                  .mat を JSON + npy に変換
      synthesize_cpp.py            出力パラメータを WAV に合成
      compare_audio.py             MATLAB の WAV と比較
    docs/
      engines.md                   2 つのエンジンの違い
      api.md                       API リファレンス
      porting-notes.md             MATLAB 版からの変更点と既知の不具合
      validation.md                検証の方法と結果

---

## 最小の使い方

```cpp
#include <tcmorph/word_tv2w_morphing.hpp>
using namespace tcmorph;

WorldParameter ref, tgt;   // WORLD 分析結果を入れる
VectorXd t_ref, t_tgt;     // 時間アンカー [s]
MatrixXd tf_ref, tf_tgt;   // 周波数アンカー (n_fanchor, n_tanchor) [Hz]

auto out = aligner::WordTV2WMorphing(
    ref, tgt, t_ref, tf_ref, t_tgt, tf_tgt,
    aligner::MorphRate::Uniform(0.5));

// out.spectrogram / out.aperiodicity / out.f0 を WORLD 合成に渡す
```

N 素材版はこちら。

```cpp
#include <tcmorph/generalized_tc_morphing.hpp>
using namespace tcmorph;

std::vector<MorphObject> objs(2);   // world_parameter と 2 種のアンカーを設定
VectorXd w(2); w << 0.5, 0.5;
auto out = GeneralizedTCMorphing(objs, MorphWeights{w, w, w, w, w});
```

重みは時間軸 `tx` / 周波数軸 `fx` / 基本周波数 `fo` / スペクトルレベル `sl` /
非周期性 `ap` の 5 系統が独立している。`fx` を `[1, 0]`、`sl` を `[0, 1]` にすれば
「フォルマント配置は A、スペクトルの傾きは B」といった組み合わせができる。

**行列の向き**: スペクトログラムは `MatrixXd(n_fbin, n_frame)`。Eigen は列優先なので
1 フレーム分が連続メモリに並ぶ。pyworld は逆の `(n_frame, n_fbin)` を返すため、
相互運用時は転置が要る。

---

## MATLAB の .mat から動かす

```bash
python3 tools/inspect_mat.py edit.mat            # 構造を確認
python3 tools/mat2json.py edit.mat out/ --names ref tgt
./bin/morph_aligner out/                         # mRate は manifest から拾う
python3 tools/synthesize_cpp.py out/
python3 tools/compare_audio.py out/ --matlab "matlab_wav/*.wav"
```

`mat2json.py` は 2 種類のレイアウト（`morphingStr` 形式と `morphdBase.morphStr` 形式）を
自動判別する。判別結果に応じて `morph_aligner` か `morph_generalized` を使う。

出力は次の 3 つに分かれる。アンカーだけが人手で編集される値なので JSON に、
機械が吐く大きな配列は npy に置いてある。

    out/anchors.json      アンカーのみ
    out/manifest.json     WORLD パラメータのメタ情報
    out/world/*.npy       WORLD パラメータ本体

---

## 移植の忠実性

MATLAB のコードを Python で直接書き下した参照実装と突き合わせ、
倍精度の丸め誤差（1e-13 台）の範囲で一致することを確認している。

実音声での検証では、MATLAB GUI が書き出した 11 条件の WAV に対し、
分析再合成の往復誤差（測定の下限）と同水準に収まり、
11 条件すべてでモーフィング率を取り違えずに判別できた。
詳細は [docs/validation.md](docs/validation.md)。

既定では MATLAB 版の既知の不具合も**そのまま再現する**。比較が目的ならこのままでよい。
`aligner::Options` のフラグで個別に修正版へ切り替えられる。
何をどう再現しているかは [docs/porting-notes.md](docs/porting-notes.md)。

---

## ライセンス

移植元の `generalizedTCmorphing.m` および `wordTV2WmorphingEngineRev.m` は
Copyright Hideki Kawahara、Apache License 2.0。本移植もそれに従う。
`LICENSE` と `NOTICE` を参照。
