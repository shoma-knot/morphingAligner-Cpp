# API リファレンス

すべて名前空間 `tcmorph` の下にある。ヘッダオンリーなのでリンクは不要。

```cpp
#include <tcmorph/tcmorph.hpp>      // 全部
#include <tcmorph/word_tv2w_morphing.hpp>   // 2 素材エンジンだけ
```

型の別名として `Eigen::VectorXd` を `VectorXd`、`Eigen::MatrixXd` を `MatrixXd`、
`Eigen::Index` を `Index` として使う。

---

## 共通のデータ構造

`generalized_tc_morphing.hpp` で定義され、両エンジンが使う。

### `SourceParameter`

WORLD の音源パラメータ。

| メンバ | 型 | 内容 |
|---|---|---|
| `temporal_positions` | `VectorXd` | フレーム時刻 [s] |
| `f0` | `VectorXd` | 基本周波数 [Hz]、無声は 0 |
| `vuv` | `VectorXd` | 1: 有声 / 0: 無声 |
| `aperiodicity` | `MatrixXd` | `(n_fbin, n_frame)` 非周期性指標 (0, 1] |

### `SpectrumParameter`

| メンバ | 型 | 内容 |
|---|---|---|
| `temporal_positions` | `VectorXd` | フレーム時刻 [s] |
| `spectrogram` | `MatrixXd` | `(n_fbin, n_frame)` パワースペクトル包絡 |
| `fs` | `double` | 標本化周波数 [Hz] |

### `WorldParameter`

| メンバ | 型 | 内容 |
|---|---|---|
| `sampling_frequency` | `double` | 標本化周波数 [Hz] |
| `span_length` | `size_t` | 分析対象波形のサンプル数。長さだけ使う |
| `source_parameter` | `SourceParameter` | |
| `spectrum_parameter` | `SpectrumParameter` | |
| `f0_original` | `VectorXd` | 編集前の F0。空なら `source_parameter.f0` で代用 |
| `duration()` | `double` | `span_length / sampling_frequency` |

`f0_original` は `WordTV2WMorphing` だけが使う。

### 行列の向き

スペクトログラムと非周期性は `(n_fbin, n_frame)`。Eigen は列優先なので
1 フレーム分のスペクトルが連続メモリに並ぶ。両エンジンとも主要な処理が
列の中で完結するため、この向きが最も効率がよい。

pyworld は `(n_frame, n_fbin)` を返すので、渡すときは `.T` が要る。

---

## N 素材エンジン

```cpp
#include <tcmorph/generalized_tc_morphing.hpp>
```

### `MorphObject`

| メンバ | 型 | 内容 |
|---|---|---|
| `world_parameter` | `WorldParameter` | |
| `time_anchor` | `VectorXd` | 時間アンカー [s]、狭義単調増加 |
| `time_freq_anchor` | `MatrixXd` | `(max_n_fanchor, n_tanchor)` 周波数アンカー [Hz] |

`time_freq_anchor` の列 `jj` が `time_anchor(jj)` におけるアンカー。
時刻によって本数が違う場合は末尾を 0 で詰める（0 の個数で本数を判定するので、
有効値に 0 Hz は使えない）。

### `MorphWeights`

`tx` / `fx` / `fo` / `sl` / `ap` の 5 本の `VectorXd`。長さはいずれも素材数。
**各ベクトルの和は 1 でなければならない**（対数領域で混ぜるため）。
和が 1 でないと `std::invalid_argument` を投げる。

| 重み | 対象 |
|---|---|
| `tx` | 時間軸（アンカー間の区間長） |
| `fx` | 周波数軸（フォルマント配置） |
| `fo` | 基本周波数。VUV にも流用される |
| `sl` | スペクトル包絡レベル |
| `ap` | 非周期性指標 |

### `MorphOptions`

| メンバ | 既定 | 内容 |
|---|---|---|
| `frame_period` | `0.005` | 出力フレーム周期 [s] |
| `vuv_threshold` | `0.99` | 有声と判定する混合 VUV の下限 |
| `vuv_output_threshold` | `0.995` | 出力 `vuv` を 1 にする下限 |
| `ap_floor` | `1e-5` | 非周期性の下限クリップ |
| `repair_nonmonotonic_frequency` | `true` | モーフ後アンカーの逆転を補正して続行する |

### `MorphOutput`

| メンバ | 内容 |
|---|---|
| `source_parameter`, `spectrum_parameter` | WORLD 合成に渡す |
| `warnings` | 計算は続行したが注意が要る点 |
| `elapsed_time` | 処理時間 [s] |
| `morphed_sgram_wo_fmod` | 周波数ワープを掛けない版（比較用） |
| `morphed_tanchor`, `morphed_tf_anchor` | モーフィング後のアンカー |
| `freq_axis_on_obj` | 素材ごとの周波数写像 |

### 関数

```cpp
MorphOutput GeneralizedTCMorphing(const std::vector<MorphObject>& objs,
                                  const MorphWeights& weights,
                                  const MorphOptions& opt = {});
```

全素材で標本化周波数・周波数ビン数・時間アンカー数・各時刻の周波数アンカー本数が
揃っていることを要求する。揃っていなければ `std::invalid_argument`。

OpenMP が有効ならフレーム方向に並列化される。

---

## 2 素材エンジン

```cpp
#include <tcmorph/word_tv2w_morphing.hpp>
```

名前空間は `tcmorph::aligner`。

### `MorphRate`

`tx` / `fx` / `fo` / `sl` / `ap` の 5 つの `double`。**0 が参照、1 が目標**。
`MorphRate::Uniform(r)` で 5 つとも同じ値にできる。

### `Options`

| メンバ | 既定 | 内容 |
|---|---|---|
| `vtl_ratio` | `1.0` | 声道長比。GUI の VTLSlider の値 |
| `fix_segment_end_index` | `false` | 区間ループの off-by-one を直す |
| `emulate_catch_dropout` | `true` | 空 `catch` によるフレーム欠落を再現 |
| `apply_aperiodicity_shaping` | `true` | 低域 −60 dB 減衰を掛ける |
| `force_all_voiced` | `true` | 全フレーム有声への固定を再現 |

既定値はすべて **MATLAB の挙動を再現する側**。詳細は
[porting-notes.md](porting-notes.md)。

### `Output`

| メンバ | 内容 |
|---|---|
| `fs`, `temporal_positions` | |
| `f0` | `f0_original * vuv` |
| `f0_original` | `vuv` を掛ける前 |
| `vuv` | `force_all_voiced` が true なら全 1 |
| `spectrogram`, `aperiodicity` | `(n_fbin, n_frame)` |
| `aperiodicity_unshaped` | 低域減衰を掛ける前 |
| `warnings` | |
| `n_dropped_frames` | 空 `catch` 相当で欠落したフレーム数 |
| `n_unsorted_interp` | 補間格子が非単調だった回数 |

### 関数

```cpp
Output WordTV2WMorphing(const WorldParameter& ref, const WorldParameter& tgt,
                        const VectorXd& t_anchor_ref, const MatrixXd& tf_anchor_ref,
                        const VectorXd& t_anchor_tgt, const MatrixXd& tf_anchor_tgt,
                        const MorphRate& rate, const Options& opt = {});
```

周波数アンカーの本数は参照側だけで決まる（MATLAB 版と同じく目標側も同数と仮定）。

---

## I/O

### アンカーの JSON

```cpp
#include <tcmorph/anchor_io.hpp>

io::AnchorSet set = io::LoadAnchorSet("anchors.json");
io::ApplyAnchorSet(set, {"ref", "tgt"}, &objs);   // 名前で対応づけ
io::SaveAnchorSet(objs, "out.json", {"ref", "tgt"});
```

JSON の形式は次のとおり。外側の添字が時間アンカー番号で、`time_anchor` と
行が 1 対 1 に並ぶ。行ごとに本数が違ってよく、短い行は自動でゼロ詰めされる。

```json
{
  "version": 1,
  "objects": [
    {
      "name": "ref",
      "time_anchor": [0.20, 0.50, 0.75],
      "time_freq_anchor": [
        [700, 1200, 2600],
        [720, 1250, 2650],
        [690, 1180, 2580]
      ]
    }
  ]
}
```

読み込み時の検証は、計算が成立しないものを例外、成立するが注意が要るものを
警告として区別する。警告は `AnchorSet::warnings` に溜まる。

| | 扱い |
|---|---|
| 時間アンカーが非単調・0 以下 | 例外 |
| 周波数アンカーが 0 以下 | 例外 |
| `time_freq_anchor` の行数不一致 | 例外 |
| 素材間でアンカー本数が不一致 | 例外 |
| 時間アンカーが発話長以上 | 例外 |
| 周波数アンカーが非単調 | 警告 |
| 周波数アンカーが Nyquist 以上 | 警告 |

### WORLD パラメータの npy

```cpp
#include <tcmorph/world_io.hpp>

io::WorldSet ws = io::LoadWorldSet("out/manifest.json");
// ws.objects[i].world_parameter / ws.names[i]

io::SaveNpy("out.npy", out.spectrogram);
```

`tools/mat2json.py` が書く npy は `(n_frame, n_fbin)` の C 連続で、これは
`MatrixXd(n_fbin, n_frame)` の列優先とメモリ配置が同一になる。転置なしに
`Eigen::Map` で読める。

対応する dtype は `<f8` / `<f4` / `<i8` / `<i4`。

---

## 例外

すべて `std::invalid_argument`。メッセージは `tcmorph: ` または
`tcmorph::io: ` で始まり、どの素材のどのアンカーが問題かまで含む。

```
tcmorph::io: objects[0](ref).time_anchor が狭義単調増加ではありません（…）
```
