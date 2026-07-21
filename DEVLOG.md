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
- `src/main.cpp` — GUI（ウィンドウ、パネル、描画、再生）
- `CMakeLists.txt` / `vcpkg.json` / `CMakePresets.json` / `vcpkg-configuration.json` — ビルド構成

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
  `out_hovered/out_held` で線ドラッグ中は新規追加を抑止。`TagX` で時間ラベル表示。
- 左クリックを追加に使うため、**パンを中ボタンに変更**（`GetInputMap().Pan = Middle`）。
  精密配置のため軸レンジは `Always`→`Once`（初期フィット後はズーム/パン可）。
- 左パネルにアンカー数表示と「全消去」ボタン。
- 注: base/target は時間が同じ値で初期化されるだけで、以後は各線を独立にドラッグして
  対応（アライメント）を編集する想定。

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

## 次にやること

- 時間軸アンカーの拡張: アンカーの**個別削除**（右クリック/選択して削除）、base↔target の
  対応が分かる表示（ペアの色分けや結線）、アンカーの整列/ソート。
- アンカーを使った**アライメント/モーフィング本体**の実装（time-warping 等）。
- 周波数軸アンカー（必要なら `DragLineY` / `DragPoint` で同様に）。
- 再生の停止/一時停止・再生位置バー（現状は `play_oneshot` で頭から再生のみ）。
- 拡大時の見た目調整（`GL_LINEAR` ↔ `GL_NEAREST`）。
- morphing/alignment 本体の実装。
