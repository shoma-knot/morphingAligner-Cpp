# morphingAligner 開発ログ

最終更新: 2026-07-21

## 最終目標

2つの音声（**base** / **target**）を読み込み、両者の**時間軸的・周波数軸的な対応をとるためのアンカー**を打てるようにする。
アンカーによる対応づけをもとに、将来的に音声モーフィング/アライメントを行う。

## アーキテクチャ / 技術スタック

| レイヤ | 使用ライブラリ | 取得方法 |
|---|---|---|
| ウィンドウ/描画 | GLFW + OpenGL3 | vcpkg（`glfw3`, `imgui[glfw-binding,opengl3-binding]`） |
| GUI / プロット | Dear ImGui 1.92 / ImPlot | vcpkg（`imgui`, `implot`） |
| ファイルダイアログ | tinyfiledialogs（ネイティブ、Linuxはzenity/kdialog） | vcpkg（`tinyfiledialogs`） |
| 音声デコード/再生 | miniaudio（C++ラッパ `ma::`） | 同梱 `third-party/miniaudio_cpp/`（pimplラッパ） |
| 音声分析 | WORLD（Harvest, CheapTrick） | git submodule `third-party/world/`（mmorise/World） |
| 日本語フォント | Noto Sans CJK（system） | `/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc` |

- C++17。ビルドは vcpkg マニフェストモード＋CMakePresets（Ninja）。
- WORLD の example ビルドは `WORLD_BUILD_EXAMPLES=OFF` で無効化（ビルド時間短縮）。

## ファイル構成（自作分）

- `src/analysis.hpp` / `src/analysis.cpp` — 音声ファイル → スペクトル包絡スペクトログラムの解析（GL非依存）
- `src/app.hpp` / `src/app.cpp` — 状態モデル（`Anchor`/`Track`/`App`）、`load_track`、
  テクスチャ生成、共有 `kColormap`
- `src/ui.hpp` / `src/ui.cpp` — 描画（`draw_left_panel`/`draw_right_panel` と内部ヘルパ、
  `EdgePoint`/`kAnchorCol`）
- `src/main.cpp` — プラットフォーム初期化（GLFW/ImGui/ImPlot）、フォント、メインループ
- `CMakeLists.txt` / `vcpkg.json` / `CMakePresets.json` / `vcpkg-configuration.json` — ビルド構成
  - `src/*` を再帰 glob（`CONFIGURE_DEPENDS`）するのでファイル追加時の CMake 変更は不要。

## 実装済み機能

### 解析パイプライン（`analysis.cpp`）
1. `ma::decoder` でファイルをデコード（interleaved float）
2. モノラル double へダウンミックス（WORLDの入力形式）
3. WORLD `Harvest` で F0 推定（`frame_period = 5.0 ms`）
4. WORLD `CheapTrick` でスペクトル包絡を推定 → `spectrogram[frame][fft_size/2+1]`（パワースペクトル）
5. `10*log10` で dB 化し、**行優先・row 0 = 最高周波数**のレイアウトで `Spectrogram` に格納
   - `Spectrogram`: `num_frames`, `num_bins`, `fs`, `duration`, `db_min`, `db_max`, `values`（float, dB）

### GUI（`main.cpp`）
- 画面を**縦に 1:4 分割**（左=操作パネル、右=表示パネル）。フルビューポートの単一ウィンドウ。
- `Track` 構造体（1音声分の状態: `name` / `path` / `status` / `spec` / `tex`）を base・target の2つ保持。
  GLテクスチャを所有するため**非コピー**。
- **左パネル**: base/target それぞれに「読み込む」「再生」ボタン＋情報（Hz, 長さ）。
  - 読み込み: `tinyfd_openFileDialog`（wav/flac/mp3/ogg）→ `analyze_file` → テクスチャ生成
  - 再生: `ma::engine::play_oneshot`（共有エンジン、頭から再生のfire-and-forget）
  - 同一ラベルのボタン衝突は `ImGui::PushID(&tr)` で回避
- **右パネル**: スペクトル包絡を**縦2段（上=base、下=target）**表示。各段に dB カラースケール凡例（`ColormapScale`）。未読み込みの段は案内テキストのみ。

### 描画の最適化（重要）
- 当初 `ImPlot::PlotHeatmap` を使用 → **毎フレーム bins×frames セルをCPUで再生成**し重かった
  （fs=44100で bins≈1025、数秒でframes数百 → 約60万セル/フレーム）。
- **テクスチャ化で解決**: 読み込み時に dB値を Viridis の256段LUTでRGBA化し、`glTexImage2D` で
  GPUテクスチャに一度だけ焼く（`make_spectrogram_texture`）。表示は `ImPlot::PlotImage` で**1クアッド**。
  → 描画コストがセル数に非依存（O(1)）。ユーザー確認済みで体感軽くなった。
- テクスチャの向き: row 0（最高周波数）が上に来るよう配置。`PlotImage` の bounds は
  `(0,0)`〜`(duration, fs/2)`、uv はデフォルト。
- カラーマップは `kColormap = ImPlotColormap_Viridis` に統一（テクスチャの色と凡例が一致）。
- 補間は `GL_LINEAR`（拡大時に滑らか。くっきり見せたい場合は `GL_NEAREST`）。

### 時間軸アンカー（`main.cpp`）
- `Anchor { double base_t; double target_t; }` を対応ペアとして `App::anchors` に保持。
- スペクトログラムを**左クリック**すると、その時間に新規アンカーを追加。
  base/target 両方に**同じ時間**で1本ずつ縦線が立つ（クリックしていない側にも打つ）。
- 縦線は **`ImPlot::DragLineX`**（ドラッグツール）で描画。静的な描画ではなく、掴んで動かせる。
  `out_hovered/out_held` で線ドラッグ中は新規追加を抑止（時間ラベルは表示しない）。
- 左クリックを追加に使うため、**パンを中ボタンに変更**（`GetInputMap().Pan = Middle`）。
  精密配置のため軸レンジは `Always`→`Once`（初期フィット後はズーム/パン可）。
  さらに `SetupAxisLimitsConstraints` で両軸を `[0,duration]×[0,fs/2]` に制約し、
  データ範囲外へのズームアウト/パンを禁止。
- 左パネルにアンカー数表示と「全消去」ボタン。
- 注: base/target は時間が同じ値で初期化されるだけで、以後は各線を独立にドラッグして
  対応（アライメント）を編集する想定。

### ペア可視化: パネル間の対応線（`main.cpp`）
- 上下のスペクトログラムにまたがって、対応する base↔target アンカーを直線で結ぶ。
- 端点の縦位置は `GetPlotPos()`/`GetPlotSize()` から得た**プロット矩形のピクセル端に固定**
  （base=下端、target=上端）。横位置のみ `PlotToPixels(x,0).x` で時間軸に追従。
  → 周波数軸をズームしても縦位置が動かない（data値0/fs/2をピクセル変換すると崩れるため固定にした）。
- `EdgePoint{pos, visible}` に格納し、`draw_right_panel` が両端点を受け取って
  `ImGui::GetForegroundDrawList()->AddLine` でスクリーン空間に結線。
  ズームで時間が表示範囲外（`GetPlotLimits` の X 範囲外）の端点はスキップ。

### 周波数軸アンカー（`ui.cpp`）
- 時間アンカー線の上に打つ周波数対応。`FreqAnchor{base_f,target_f}` を `Anchor::freqs` に保持。
- 各パネルの線上に **`ImPlot::DragPoint`** で表示。X は親の時間アンカー時刻に毎フレーム固定し、
  Y（周波数）だけ動かす。`ImPlotDragToolFlags_Delayed` でドラッグ中も X が線上に留まる。
- 操作モードを Ctrl で切替（同じ x 上の線と点でドラッグ対象が競合するため）:
  - Ctrl なし … 時間線ドラッグ可 / 周波数点ロック（`NoInputs`）
  - Ctrl あり … 時間線ロック（`NoInputs`）/ 周波数点ドラッグ可
  - Ctrl+左クリック（点以外の線上, ピクセル距離で最寄り線を判定）で追加、
    Ctrl+左ドラッグで移動、Ctrl+右クリックで削除。
- `main.cpp` で `GetInputMap().OverrideMod = ImGuiMod_None`（既定の Ctrl=DnD/入力無視を解除）。
- 番号は時間アンカー内の並び順（`j+1`）を `Annotation` で表示。base/target が同番号＝対応。
  表示範囲（`GetPlotLimits`）外の点はラベルを出さない。

### アンカーの保存/読み込み（`session.cpp`）
- `nlohmann-json` で JSON 保存/読み込み。スキーマ:
  ```json
  { "version":1,
    "waves": {"base":"...","target":"..."},
    "anchors":[ {"time":{"base":..,"target":..},
                 "freqs":[{"base":..,"target":..}]} ] }
  ```
- freqs は base/target のペア配列（内部モデルと一致、長さ食い違いが起きない）。
- 読み込みは JSON パース → waves 取得 → 音声の存在確認 → アンカー検証 → 音声復元 →
  アンカー適用の順。音声が見つからない/開けない場合はエラー表示して中止（状態を壊さない）。
- `load_track` からパス指定版 `load_track_from_path` を抽出し共用。
- 左パネルに保存/読み込みボタン。既定パスはカレントディレクトリ
  （実行ファイルのパス取得は OS 固有になるため移植性優先）。

### モーフィング（`morphing.cpp`）
Kawahara の generalizedTCmorphing.m を参考に、2ソース(base/target)＋軸ごとの率
(tx=時間 / fx=周波数 / fo=F0 / sl=スペクトル / ap=非周期性)で実装。UI は一律スライダ
（`MorphRates::uniform(r)` で全軸同値を渡す）。
- 解析: `ma::decoder`→mono→WORLD Harvest(F0)+CheapTrick(sp)+D4C(ap)。base/target は
  同一 fs 前提（fft_size も一致）。
- 時間軸: 時間アンカーを base_t 昇順に整列＋両端に境界(0,0)/(dur,dur)を追加。
  セグメント長を log 補間して morphed timeline を作り、逆写像で各元の時刻を得る。
- F0: log 補間、voicing は重み閾値。無声側が優勢なら 0。
- 周波数軸: 各時間アンカーごとに「モーフ周波数→base/target 周波数」の全ビン写像を前計算し
  （`build_freq_warp`）、区間内で左右アンカーをビンごとに `s` 補間（時間方向に連続、MATLAB の
  `(1-lambda)*last + lambda*next` 相当）。周波数アンカーの本数が時間アンカー間で違っても可。
  sp は log 補間、ap は線形補間。
- 合成: WORLD Synthesis。WAV 出力は miniaudio のエンコーダ（`ma::write_wav`, 32bit float）。
- API: `MorphResult morphing(base_path, target_path, anchors, const MorphRates&)`。
  各軸は該当箇所で対応する率を使用（tx=timeline, fo=morph_f0, fx=周波数折れ線,
  sl=sp のlog補間, ap=ap の線形補間）。
- UI: 左パネルに率スライダ＋「生成して再生」（cwd/morph.wav に書いて play_oneshot）。
- ヘッドレス検証済み（JVS 2話者、r=0→base長, r=1→target長, 全ケース有限出力）。
- 既知の制約: (1)同期実行で数秒 UI が固まる。(2)fs 不一致は未対応。(3)F0 は最近傍
  フレームサンプル。MATLAB との差分は下の「## MATLAB版との差分」を参照。

## MATLAB版との差分

`generalizedTCmorphing.m` を精読して現状実装と比較した結果（2026-07-21）。

**一致している点**
- 時間軸: セグメント長の重み付き幾何平均（log 補間）＋ cumsum。
- フレーム周期 5ms、時間の逆写像はアンカー間で区分線形。
- スペクトルの合成は log 領域の重み補間（幾何平均）→ exp。
- 周波数ワープの時間方向補間（左右アンカーを区間内で補間）。← 今回対応。
- 正規化/プリエンファシス/ゲイン補正なし。

**MATLAB に合わせて対応済み（2026-07-21）**
1. ✅ **周波数軸を log 周波数化**（`build_freq_warp`）。アンカー位置を幾何平均、逆写像も log 周波数で線形。
2. ✅ **sp/ap の補間を log 領域に**（`sample_log`）。log 値を時間・周波数とも bilinear 補間。
3. ✅ **非周期性(ap)の合成を log 領域に**（clamp `[1e-5,1]`→log→重み→exp）。
4. ✅ **F0**: 時間補間を log 線形に、voicing 閾値を **0.99** に（VUV を線形補間して重み和→閾値）。

**未対応（スコープ差／軽微）**
5. **多オブジェクト＋軸別重みベクトル**: MATLAB は N ソース各々に軸別重み。現状は 2 ソース＋
   `(1-r, r)`（機能的にはその部分集合）。
6. 境界: MATLAB は線形 extrap／現状は端クランプ（軽微）。

### 動作ログ領域（`log.cpp` / `ui.cpp`）
- グローバルな `applog`（`add`/`lines`/`clear`、`[HH:MM:SS]` 付き）を導入。
- `Track::status`・`App::session_status`・`App::morph_status` を廃止し、読み込み/再生/セッション/
  モーフィングの結果はすべて `applog::add` に集約（各パネルの個別ステータス表示は撤去）。
- レイアウトを `draw_root` に集約し縦 **8:2**（上=操作/表示 左1:右4、下=ログ）。ログは横スクロール可、
  最下部にいるとき自動追従。`ui.hpp` の公開は `draw_root` のみ。`main.cpp` は `draw_root(app)` を呼ぶだけ。

## ビルド / 実行

```sh
export VCPKG_ROOT="$HOME/.local/vcpkg"
cmake --preset vcpkg      # 初回は依存インストールで数分
cmake --build build
./bin/main
```

## 既知の制約・メモ

- **この開発環境（Wayland）ではGUIのスクリーンショットが撮れない**（scrot/importは黒画面、grim未導入）。
  アプリ自体は正常動作。目視確認はユーザーが `./bin/main` を起動して行う。
- ビルド/起動の検証は「数秒起動してstderrにエラーが出ないこと」で代替している。

## 注意点（fragile）

- **周波数軸の手組み入力処理**（`ui.cpp` の `FIXME(freq-axis-input)` ブロック）:
  ホイールは X 専用ズームにするため Y 軸を `Lock` し、Y の表示範囲は `tr.y_min/y_max` に
  自前で保持して毎フレーム `SetupAxisLimits(Always)` で再適用している。ImPlot の軸状態を
  複製しているため壊れやすい（`IsPlotHovered` ゲートが2段プロット間のドラッグ跨ぎで誤作動、
  Lock 解除や Y 軸追加でデシンク等）。拡張時は自前処理を伸ばさず ImPlot に軸を任せる方向で。

## 次にやること

- 時間軸アンカーの拡張: アンカーの**個別削除**（右クリック/選択して削除）、base↔target の
  対応が分かる表示（ペアの色分けや結線）、アンカーの整列/ソート。
- アンカーを使った**アライメント/モーフィング本体**の実装（time-warping 等）。
- 周波数軸アンカー（必要なら `DragLineY` / `DragPoint` で同様に）。
- 再生の停止/一時停止・再生位置バー（現状は `play_oneshot` で頭から再生のみ）。
- 拡大時の見た目調整（`GL_LINEAR` ↔ `GL_NEAREST`）。
- morphing/alignment 本体の実装。
