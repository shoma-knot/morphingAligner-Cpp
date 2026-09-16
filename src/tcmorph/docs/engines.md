# 2 つのエンジンの違い

worldGUItools には音声モーフィングのエンジンが 2 つあり、GUI ごとに使い分けられている。
**両者は別物で、同じアンカーを与えても違う音になる。** 移植もそれぞれ別に用意してある。

| GUI | エンジン | 保存する構造 | tcmorph の関数 |
|---|---|---|---|
| `morphingAligner` | `wordTV2WmorphingEngineRev` | `morphingStr` | `aligner::WordTV2WMorphing` |
| `morphingSoundGenerator` | `wordTV2WmorphingEngineRev` | — | 同上 |
| `morphContinuumGen` | `generalizedTCmorphing` | `morphdBase` | `GeneralizedTCMorphing` |
| `threeItemMorpherGeneralUI` | `generalizedTCmorphing` | `morphdBase` | 同上 |

手元の `.mat` がどちらかは `tools/inspect_mat.py` で分かる。トップレベルに
`morphingStr`（`worldPRef` / `worldPTgt` を含む）があれば前者、
`morphdBase.morphStr` があれば後者。`tools/mat2json.py` は自動判別して
`manifest.json` の `layout` に記録する。

---

## 違いの一覧

| 項目 | `GeneralizedTCMorphing` | `aligner::WordTV2WMorphing` |
|---|---|---|
| 素材数 | N 個 | 2 個（参照 / 目標）固定 |
| 重み | 素材ごとのベクトル | スカラー（0 が参照、1 が目標） |
| F0 の取得元 | `source_parameter.f0` | `f0_original` |
| F0 の混合 | Hz 領域の算術平均 | 幾何平均 |
| 周波数区間長の混合 | log 差の算術平均 | log 差の幾何平均 |
| VUV | 閾値 0.99 / 0.995 で二値化 | 計算後に全フレーム有声へ固定 |
| 非周期性 | そのまま | 低域を最大 −60 dB 減衰 |
| 発話長の終端 | `length(span)/fs` | `temporal_positions(end)` |
| 第 0 ビンの周波数 | 半ビン分ずらす | 0 Hz のまま |
| Nyquist ビン | そのまま | 直前のビンで上書き |
| 声道長補正 | なし | `vtl_ratio` |
| 周波数写像の向き | モーフ後 → 素材（backward） | 素材 → モーフ後（forward） |

---

## 数値でどれくらい違うか

### F0 の混合

```
120 Hz と 220 Hz を rate 0.5 で混ぜた場合
  GeneralizedTCMorphing      170.00 Hz   (算術平均)
  aligner::WordTV2WMorphing  162.48 Hz   (幾何平均)
  差 78.3 cent
```

端点（0 と 1）では一致するが、中間では半音近くずれる。

### 周波数アンカーの混合

`GeneralizedTCMorphing` では、モーフィング後のアンカーは各素材の対応する
アンカーの重み付き**幾何平均**になる。対数区間長の重み付き和を累積すると
途中の項が打ち消し合い、`Π aᵢ^wᵢ` に帰着するため。

```
A = [700, 1200, 2600], B = [850, 1600, 3000], rate 0.5
  GeneralizedTCMorphing      [771.4, 1385.6, 2792.8]
  aligner::WordTV2WMorphing  [770.8, 1382.1, 2775.3]
```

`WordTV2WMorphing` は log 区間長どうしを幾何平均するので、値が少し違う。

### 非周期性の低域整形

`WordTV2WMorphing` はモーフィング後に、平均 F0 以下の帯域の非周期性を
−60 dB 減衰させ、3×平均 F0 まで余弦で滑らかに戻す。低域を強制的に
周期的にする処理で、`GeneralizedTCMorphing` には無い。
出音の差としては最も大きい要素のひとつ。

`Options::apply_aperiodicity_shaping = false` で無効にできる。
`Output::aperiodicity_unshaped` に整形前の値も入っている。

---

## 逆転したアンカーの扱い

同じ時刻の周波数アンカーが逆転している（GUI でアンカー線が交差した）場合、
2 つのエンジンは違う振る舞いをする。

**`WordTV2WMorphing`** は `real(exp((1-w)log a + w log b))` の形で計算する。
`a` が負だと `log(-|a|) = log|a| + iπ` なので、実部を取ると

```
exp((1-w)log|a| + w log|b|) · cos(π·((a<0)(1-w) + (b<0)w))
```

となり、片方だけ負なら `cos(π/2) = 0` でちょうど 0 になる。逆転した帯域が
幅ゼロに潰れ、`safeguard`（0.0001 刻みのランプ）で分離される。
本移植もこの挙動をそのまま再現する。

**`GeneralizedTCMorphing`** は区間長の符号をそのまま保つので、逆転が
重みで打ち消されないとモーフィング後のアンカーも逆転しうる。その場合は
対数周波数で最小間隔だけ押し広げて続行し、警告を出す
（`MorphOptions::repair_nonmonotonic_frequency = false` で例外にできる）。

なお素材ごとのアンカーが逆転していること自体は誤りではない。
これらは補間の y 側に入るだけで、折り返すワープ関数になるだけである。
単調性が本当に要求されるのはモーフィング後のアンカー位置（x 側）。

---

## どちらを使うべきか

手元の音声を再現したいなら、それを作った GUI に対応するほうを使う。
新規に組むなら、素材が 2 つで済み、GUI と同じ音を出したいなら
`WordTV2WMorphing`、素材を 3 つ以上混ぜたい・属性ごとに細かく重みを
変えたいなら `GeneralizedTCMorphing` が向く。
