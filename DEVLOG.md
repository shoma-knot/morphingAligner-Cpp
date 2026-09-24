# morphingAligner 開発ログ

最終更新: 2026-09-24（現行版 v26.09.17）

## 最終目標

2つの音声（**base** / **target**）を読み込み、両者の**時間軸的・周波数軸的な対応をとるためのアンカー**を打てるようにする。
アンカーによる対応づけをもとに音声モーフィングを行う（モーフィング本体は tcmorph で実装済み）。

## 現状の要約（2026-09-24 時点）

- タブ構成: **アライメント**（アンカー編集）／**モーフィング**（5軸スライダー＋3×3 プロット＋再生/WAV 保存）／**ライセンス表示**。下部に全タブ共通のログ。
- モーフィングエンジンは同梱の **tcmorph**（Kawahara の `wordTV2WmorphingEngineRev.m` の C++ 移植）の
  `aligner::WordTV2WMorphing`。自前実装（下の「モーフィング（旧・自前実装）」「MATLAB版との差分」）は廃止済み。
- セッションは独自 JSON に加え、tcmorph の `anchors.json`（アンカーのみ）も読める。
- **Python 連携**（2026-09-24）: `./.env` の Python を子プロセスで呼び、フォルマント（parselmouth）を
  スペクトログラムに重ね、音素セグメンテーション（MFA）を時間軸を共有した独立プロットに出す。
- 配布: `v*` タグの push で GitHub Actions が Ubuntu / Windows 版をビルドしリリースに添付する。
- 以下の「実装済み機能」は時系列で追記しているため、前半の節には後で置き換わった記述がある
  （置き換わった箇所には注記を入れてある）。

## アーキテクチャ / 技術スタック

| レイヤ | 使用ライブラリ | 取得方法 |
|---|---|---|
| ウィンドウ/描画 | GLFW + OpenGL3 | vcpkg（`glfw3`, `imgui[glfw-binding,opengl3-binding]`） |
| GUI / プロット | Dear ImGui 1.92 / ImPlot | vcpkg（`imgui`, `implot`） |
| ファイルダイアログ | tinyfiledialogs（ネイティブ、Linuxはzenity/kdialog） | vcpkg（`tinyfiledialogs`） |
| 音声デコード/再生 | miniaudio（C++ラッパ `ma::`） | 同梱 `third-party/miniaudio_cpp/`（pimplラッパ） |
| 音声分析/合成 | WORLD（Harvest, CheapTrick, D4C, Synthesis） | git submodule `third-party/world/`（mmorise/World） |
| モーフィング | tcmorph（ヘッダオンリー、Eigen 依存） | 同梱 `tcmorph/`（Apache 2.0） |
| 行列演算 | Eigen | vcpkg（`eigen3`。版は `vcpkg-configuration.json` の baseline で固定） |
| セッション JSON | nlohmann-json | vcpkg（`nlohmann-json`） |
| 日本語フォント | Gen Interface JP Regular（OFL v1.1） | 同梱 `font/Gen Interface JP/` |
| フォルマント | parselmouth（Praat） | `./.env` の Python（micromamba、git 管理外） |
| 音素セグメンテーション | Montreal Forced Aligner 3.4（japanese_mfa） | 同上（手順は「ビルド / 実行」） |

- C++17、CMake 4.0 以上。ビルドは vcpkg マニフェストモード＋CMakePresets（Ninja）。
- WORLD の example ビルドは `WORLD_BUILD_EXAMPLES=OFF` で無効化（ビルド時間短縮）。
- 版は `CMakeLists.txt` の `project(... VERSION 26.09.17)`。`APP_VERSION` としてコンパイル定義で渡し、
  タイトルバーに `morphingAligner v<版>` と出す（CMake は先頭ゼロを正規化しないので `26.09.17` のまま）。

## ファイル構成（自作分）

- `src/analysis.hpp` / `src/analysis.cpp` — 音声ファイル → 表示用スペクトル包絡スペクトログラムの解析（GL非依存）
- `src/app.hpp` / `src/app.cpp` — 状態モデル（`Anchor`/`FreqAnchor`/`Track`/`App`）、`apply_track`、
  テクスチャ生成（ERB 等間隔）、共有 `kColormap`
- `src/ui.hpp` / `src/ui.cpp` — 画面全体（`draw_root`）。タブ、左パネル、スペクトログラム、ミニマップ、
  モーフィングタブ、ライセンス表示、ログ、非同期ジョブ（`launch_ui_job` / `request_morph`）
- `src/anchors.hpp` / `src/anchors.cpp` — 時間/周波数アンカーの描画・操作、パネル間の対応線
- `src/morphing.hpp` / `src/morphing.cpp` — WORLD 解析（`analyze_channel`）と tcmorph によるモーフィング
  （`morphing_channels`）、WAV 書き出し
- `src/session.hpp` / `src/session.cpp` — セッション JSON の保存/読み込み（tcmorph のアンカー形式も読む）
- `src/log.hpp` / `src/log.cpp` — スレッドセーフな動作ログ `applog`
- `src/freqscale.hpp` — Hz ↔ ERB レート変換
- `src/speech_tools.hpp` / `src/speech_tools.cpp` — Python ツールの呼び出し（子プロセス起動・JSON の
  要求/応答・終了時の停止）と結果の型（`Formants` / `Segmentation`）
- `src/speech_view.hpp` / `src/speech_view.cpp` — フォルマントの重ね描き、音素セグメンテーションのプロット
- `python/speech_tools.py` — フォルマント推定（parselmouth）と音素セグメンテーション（MFA）の本体
- `src/app_icon.*` / `src/app_icon_data.inc` / `src/app_icon.rc.in` — ウィンドウアイコン（埋め込み RGBA）と
  Windows 用リソース（.ico を実行ファイルに埋め込む）
- `tcmorph/` — モーフィングエンジン（ドキュメントは `tcmorph/README.md`, `tcmorph/docs/`）
- `tools/gen_icon.py` — アイコン（`icon/*.png`, `icon/morphingaligner.ico`, `src/app_icon_data.inc`）の生成
- `tools/install-desktop-entry.sh` / `uninstall-desktop-entry.sh` — Linux のデスクトップエントリ登録/解除
- `.github/workflows/release.yml` — タグ push で配布物をビルドしてリリース
- `CMakeLists.txt` / `vcpkg.json` / `CMakePresets.json` / `vcpkg-configuration.json` — ビルド構成
  - `src/*.cpp` を**直下のみ** glob（`CONFIGURE_DEPENDS`）。ファイル追加時の CMake 変更は不要。
    再帰 glob にすると tcmorph の examples（`main()` を持つ）を巻き込むので不可。

## 実装済み機能

### 解析パイプライン（`analysis.cpp`）
1. `ma::decoder` でファイルをデコード（interleaved float）
2. モノラル double へダウンミックス（WORLDの入力形式）
3. WORLD `Harvest` で F0 推定（`frame_period = 5.0 ms`）
4. WORLD `CheapTrick` でスペクトル包絡を推定 → `spectrogram[frame][fft_size/2+1]`（パワースペクトル）
5. `10*log10` で dB 化し、**行優先・row 0 = 最高周波数**のレイアウトで `Spectrogram` に格納
   - `Spectrogram`: `num_frames`, `num_bins`, `fs`, `duration`, `db_min`, `db_max`, `values`（float, dB）

### GUI（`main.cpp`）
※ 初期実装の記録。現在の描画は `ui.cpp`（`draw_root`）、アンカーは `anchors.cpp` に分離済み。
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
- 番号は時間アンカー内の並び順（`j+1`）。base/target が同番号＝対応。
  表示範囲（`GetPlotLimits`）外の点はラベルを出さない。
  ※ 2026-09-16 に自前描画＋カーソル近傍の1本のみ表示へ変更（下の「アンカーの視認性の改善」）。

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

### モーフィング（旧・自前実装、`morphing.cpp`）
※ **2026-09-16 に tcmorph へ差し替えて廃止**（下の「モーフィングエンジンを tcmorph に差し替え」）。
UI も後にモーフィングタブへ移動している。以下は当時の記録。

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
- UI: 左パネルに率スライダ＋「生成して再生」（結果を `ma::engine::play_pcm` でメモリから
  直接再生。ファイルは書かない）＋「結果を WAV 保存」（ダイアログで明示保存のときだけ書く）。
  再生用に miniaudio_cpp へ `play_pcm`(ma_audio_buffer 使用)を追加。
- ヘッドレス検証済み（JVS 2話者、r=0→base長, r=1→target長, 全ケース有限出力）。
- 既知の制約: (1)同期実行で数秒 UI が固まる。(2)fs 不一致は未対応。(3)F0 は最近傍
  フレームサンプル。MATLAB との差分は下の「## MATLAB版との差分」を参照。

### スペクトログラムの周波数軸を ERB 尺度に（`freqscale.hpp` ほか）
- 表示の周波数軸を **ERB レート**尺度に（低域が広がり聴覚的に自然）。`freqscale::hz_to_erb`/
  `erb_to_hz`（Glasberg & Moore 1990）に変換を集約。
- テクスチャ(`app.cpp`)は各行を ERB 等間隔で再サンプル。Y軸座標は ERB レート、目盛りは
  `SetupAxisFormat` で Hz 表示。目盛り位置は **1-2-5 系列**（100,200,500,1000,…）で高域の密集を回避。
- `tr.y_min/y_max` と周波数アンカーの表示/入力（`DragPoint`）も ERB 基準に（アンカー値自体は Hz 保持
  なのでセッション互換。モーフィング処理は Hz のままで無関係）。

### ミニマップ（`ui.cpp`）
- スペクトログラムに VSCode 風ミニマップ（全体表示＋現在表示範囲の白半透明ボックス）。
  `draw_minimap` は `CanvasOnly | NoInputs`、軸装飾なしで全体を PlotImage。枠は前フレームの
  メイン表示範囲 `Track.view_*`（X=秒, Y=ERB）を `GetPlotDrawList` で矩形描画。
- 配置は `enum Minimap{None,Above,Below}`。base=上/target=下。本体はミニマップ分だけ縮め、
  base/target の本体高さは一致。左パネルの「ミニマップを表示」チェックボックス（既定 OFF）で切替。

### タブ化とモーフィングタブ（`ui.cpp` / `morphing.*` / `app.*`）
- 画面上部を `TabBar` で **「アライメント」/「モーフィング」** に分割（ログは全タブ共通の下部）。
- **モーフィングタブ（縦 4:4:1）**:
  - 上=**5軸スライダー**（tx/fx/fo/sl/ap、「全軸を一括操作」チェックで1本化）。
    スライダーを**離したタイミングで再合成**（`IsItemDeactivatedAfterEdit`）。
  - 中=**3×3 プロット**（列: Base/Morphed/Target × 行: F0 ライン / sp / ap ヒートマップ）。
    `BeginSubplots(LinkAllX|LinkRows)` で軸共有・隙間最小。目盛りラベルは左端列と最下行のみ、
    タイトルは最上行のみ。X=base/target の長い方、F0=共通最大×1.1、sp/ap=ERB 全域。
    sp は 3枚共通の dB レンジ、ap は [0,1] 固定で色スケールも共通。
  - 下=出力設定（「生成して再生」「再生」「WAV 保存」）。生成時間を %.2f ms でログ。
- `morphing_full()` を追加し base/target/morphed の f0/sp/ap（`MorphChannel`/`MorphOutput`）を公開。
  既存 `morphing()` は wave のみ返すラッパに（共通実装 morph_impl の want_data フラグ）。
- sp/ap の6枚のテクスチャは `rebuild_morph_textures`（app.cpp、ERB 等間隔の汎用
  `make_heatmap_texture`）で生成、`~App` で解放。
- 左パネルのモーフィング操作は撤去しタブへ集約（`App::morph_rate/morph_wave/morph_fs` →
  `morph_rates/morph_link/morph_out` に整理）。

### モーフィングタブの改善（2026-07-23）
- **base/target と morphed の分離**: `analyze_channel(path)`（1音源の解析）と
  `morphing_channels(base_ch, target_ch, anchors, rates)`（解析済みチャンネルから morphed のみ
  合成）に API を再構成（旧 `morphing`/`morphing_full` は廃止、MorphOutput は morphed+wave のみ）。
  - base/target はタブ表示時に `ensure_morph_channels` が解析（パス変更時のみ再解析、失敗パスは
    記録して再試行を防ぐ）。**音声を読み込めば生成前でも base/target のプロットが出る**。
  - 再合成は morphed のみ（`rebuild_morphed_texture`）。**解析が走らなくなり大幅に高速化**。
  - sp 共通 dB レンジは base/target から算出（morphed は log 補間なので必ずレンジ内）。
  - ヘッドレスでリファクタ前後の出力一致を確認（r=0/0.5/1 の長さ・maxabs 同一）。
- **再合成トリガ**: スライダーの値が変わった各フレームで再合成。「リアルタイム更新」
  チェックボックス（既定 ON）で OFF=離した時のみ、に切替可能（低スペック環境向け）。
- **UI 調整**: ログ領域を従来の 2/3 に縮小し `CollapsingHeader` で折りたたみ可能に
  （`App::log_open`、高さは前フレームの開閉状態で決定）。グリッドの時間軸ラベルを非表示。
  出力設定とスライダー部をコンテンツ高さに（スライダーは一括切替でサイズが変わらないよう
  常に5本分を確保）。「全軸を一括操作」をヘッダ行へ移動。スライダーは最長ラベル基準の
  共通オフセットで中央揃え（バー位置が行間で揃う）。

### モーフィングの別スレッド化（2026-07-23）
- `request_morph`（std::async でワーカー起動）＋`poll_morph_job`（draw_root で毎フレーム完了回収）。
  GL テクスチャ更新・ログ・再生はメインスレッド側で実施。
- 実行中の再要求は pending に畳み、完了後に**最新条件で1回だけ**再実行（コアレス）。
- base/target チャンネルは `shared_ptr<const MorphChannel>` で共有（解析し直しで差し替わっても
  実行中ジョブは自分の参照を保持）。アンカー/率はジョブ起動時にコピー。
- 世代カウンタ `morph_epoch` で、base/target 差し替え後に完了した古い結果を破棄。
- 出力行に「生成中...」表示。スライダー操作・生成で UI がブロックしなくなった。
- main に Threads::Threads を明示リンク。非同期実行の出力一致をヘッドレスで検証済み。

### ファイル読み込み・ダイアログの別スレッド化（2026-07-23）
- 汎用 UI ジョブ: `launch_ui_job`（ワーカーでダイアログ＋重い処理を実行し「メインスレッドで
  適用する処理」を返す）＋`poll_ui_job`（draw_root で毎フレーム回収）。同時1本、実行中は
  関連ボタンとタブ切替を無効化（選択中タブの中身は操作可能なまま）。
- 非同期化: 音声「読み込む」（ダイアログ＋analyze_file→apply_track）、セッション保存
  （ダイアログのみワーカー）、セッション読み込み（ダイアログ＋JSON＋両音声解析→
  apply_session_data）、WAV 保存（ダイアログ＋書き出し）、モーフ用 base/target 解析。
- 下回り: applog をスレッドセーフ化（mutex、lines() はスナップショット返し）。
  `load_track/load_track_from_path` → `analyze_file`(worker)＋`apply_track`(main) に分離。
  `load_session` → `load_session_data`(worker-safe)＋`apply_session_data`(main) に分割。
- GL（テクスチャ）と App の状態変更は必ず適用クロージャ＝メインスレッドで実行する規約。
- tinyfd はダイアログを閉じるまでブロックするため、ワーカー実行で「応答なし」を解消
  （Linux では zenity/kdialog のサブプロセスなので非メインスレッドで安全）。

### フォント同梱とライセンス表示タブ（2026-07-23）
- **フォント同梱**: Windows ビルドでシステムフォントが見つからず文字化けするため、
  Gen Interface JP Regular（OFL v1.1、Inter ベースの日本語フォント）を `font/` に同梱。
  `main.cpp` の候補パスはカレント起動と bin/ 起動の2つ（システムフォントのフォールバックは削除）。
  OFL はソフトウェアへの同梱・再配布を明示的に許可（OFL.txt の同梱が条件、フォント単体販売のみ禁止）。
- **ライセンス表示タブ** (`draw_license_tab`): 左=同梱物リスト(1) / 右=条文表示(4)。
  条文はファイルから遅延読み込みしてキャッシュ。タブは機能タブと区別するためグレー系
  （`ImGuiCol_Tab*` 5色を PushStyleColor）。※右端寄せ（Trailing や自前タブ風ボタン）は
  試したが見た目/挙動が安定せず断念。
- **同梱条文の選定**: バイナリ配布時に条文明記が必要なもののみ `licenses/` に同梱
  （Dear ImGui/ImPlot/nlohmann JSON=MIT、WORLD=修正BSD、＋フォントの OFL.txt）。
  zlib 系（GLFW, tinyfiledialogs）と public domain/MIT-0（miniaudio）は義務がないため省略。
- **条文の等幅表示**: 条文は等幅前提の整形なので、ImGui 埋め込みの ProggyClean を第2フォント
  として追加（`load_fonts()` → `App::mono_font`）し、条文本文だけ `PushFont` で切り替え。
  追加ファイル・追加ライセンス不要（imgui 同梱・MIT）。vcpkg の imgui ポートは
  `misc/fonts/` の TTF（Roboto, Cousine 等）をインストールしない点に注意。

### モーフィングエンジンを tcmorph に差し替え（2026-09-16）
- 自前のワープ/補間実装を廃し、`tcmorph::aligner::WordTV2WMorphing`（`wordTV2WmorphingEngineRev.m`
  の移植）を使う。本アプリは morphingAligner 相当（2素材）なので N 素材の `GeneralizedTCMorphing`
  ではなく aligner 側。オプションは既定（MATLAB の挙動を再現する側、声道長比 1.0）。
- `MorphChannel` は `tcmorph::WorldParameter` を直接保持。sp/ap は Eigen の `MatrixXd(nbin, n_frames)`
  列優先で、CheapTrick/D4C へ各列の先頭ポインタをそのまま渡す（コピー不要）。
- アンカーの整形（`build_anchors`）: tcmorph は時間アンカーの**狭義単調増加**を要求する。崩れると
  モーフ後タイムラインが後戻りし広帯域ノイズになるため、範囲外・重複を落としたうえで target 側が
  単調になる最大部分列を **DP** で選ぶ（貪欲だと1本の交差で以降が全滅する）。周波数アンカーは
  base_f 昇順に並べ替え、`(本数, 時間アンカー数)` の 0 詰め行列にする。範囲は最終フレーム時刻基準。
- 読み飛ばしたアンカー数とエンジンの警告は `MorphOutput::warnings` → ログ（内容が変わったときだけ）。
- 合成長は最終フレーム時刻ちょうど（MATLAB の Synthesis と同じ長さ）。
- 検証: rate 0/0.25/0.5/0.75/1 で NaN・フレーム欠落なし。rate 0/1 と元音声の短時間パワー包絡の
  相関 0.994 / 0.996。アンカー0本・交差・範囲外・0Hz でも例外なし。

### tcmorph 形式のアンカー JSON の読み込み（2026-09-16）
- 「セッション読み込み」はトップレベルに `objects` があれば tcmorph 形式（`anchor_io.hpp`）と判定。
  音声パスを持たないので、**現在の base/target にアンカーだけを乗せる**（未読み込みなら中止）。
- `objects[0]`=base、`objects[1]`=target。形式検証は `tcmorph::io::ParseAnchorSet` に任せる。
  周波数アンカーの 0 詰めは落とす。
- 本アプリは周波数アンカーを base 側周波数順に並べ替えるため、逆転したアンカーを含むファイルは
  tcmorph 単体と結果が変わる（読み込み時にログで補足）。

### パス表示と「離したら再生」（2026-09-16）
- 左パネルの Base/Target はファイル名のみ表示（フルパスはツールチップ）。
- モーフィングタブに「離したら再生」（既定 OFF）。再生予約を `morph_play_request`（次に開始する
  ジョブ）と `morph_job_play`（実行中ジョブ、開始時に確定）に分け、pending に畳まれた古い率の
  結果が再生を横取りしないようにした。リアルタイム更新 ON でも離したフレームで再合成を要求する。

### アンカーの視認性の改善（2026-09-16、`anchors.cpp`）
- 番号ラベルは**カーソル近傍の時間アンカー1本ぶんだけ**表示。対象は「ドラッグ中の線 → ホバー中の
  周波数点 → 最も近い線（15px 以内）」の順。base/target で共有するため、今フレームのホバーを
  `App::hover_anchor` に集めて次フレームの `active_anchor` にする（1フレーム遅延）。
- 同じ線上のラベルは画面 Y でソートして最小すき間だけ押し広げ、引き出し線を必ず引く
  （`ImPlot::Annotation` は位置を厳密に決められないので同じ見た目を自前描画）。
- 対応線は常に全部を α0.35 で描き、active の1本だけ不透明。時間線は 1.0px、active のみ 2.0px。
  点は 3.5px。※「1本強調＋残り減光」は一斉に明滅してうるさかったので不採用。

### Ctrl+ホイールで周波数軸ズーム（2026-09-16）
- プロット領域上で Ctrl+ホイール → Y（周波数）ズーム（従来は軸ラベル上のみ）。「Ctrl=周波数方向」で
  アンカー操作と揃う。Ctrl 中は X 軸も Lock して時間軸ズームの同時発火を防ぐ（副作用: Ctrl 中は X の
  中ドラッグパンも止まる。許容）。

### アプリアイコン（2026-09-17）
- 意匠は `tools/gen_icon.py` が持ち、`icon/*.png`・`icon/morphingaligner.ico`・`src/app_icon_data.inc`
  を生成。ウィンドウアイコン（16/32/48/64px RGBA）は実行ファイルに埋め込み `glfwSetWindowIcon` へ。
- Windows: `.ico` を `app_icon.rc.in`（configure_file で絶対パス展開）で実行ファイルに埋め込む。
  rc.exe には `/c65001` を渡す（UTF-8 コメントが化けるため）。.ico は 128px 未満 BMP / 以上 PNG。
- Linux(GNOME): タスクバーは WM_CLASS で .desktop に紐づけるため、`tools/install-desktop-entry.sh`
  で登録（外に出るのはシンボリックリンク1個、`Path=` で作業ディレクトリを固定、`NoDisplay=true`）。
- WM_CLASS / Wayland app_id は `main.cpp` で `morphingAligner` に固定（タイトルに版を入れても
  `StartupWMClass` と一致させるため）。

### 配布・リリース（2026-09-17）
- 実行ファイル名を `morphingAlignerCpp`（Windows は `.exe`）に変更。
- `.github/workflows/release.yml`: `v*` タグ push で linux-x64 / windows-x64（`x64-windows-static`、
  静的 CRT）をビルドし、tar.gz / zip を GitHub リリースに添付。両環境 Ninja（Windows は
  `ilammy/msvc-dev-cmd`）、CMake は `lukka/get-cmake`、vcpkg バイナリキャッシュあり。
- 配布物は実行ファイルを直下に置き、`font/` `licenses/`（Linux は `icon/` `tools/` も）を同梱。
  中身の検査（必要ファイル・ldd の未解決）もワークフロー内で行う。
- タグと `project()` の VERSION が食い違うとビルドを止める（`-rc1` などの接尾辞は可）。
- MSVC では `CMAKE_CXX_FLAGS` を上書きしない（`/EHsc` が消えて例外処理が壊れるため）。
- Eigen は `find_package(Eigen3 CONFIG REQUIRED)`（版指定なし）。vcpkg の eigen3 は 5.0.1 を名乗り
  3.4 指定を満たさないため。3.4.0 と 5.0.1 で WAV がバイト一致することを確認済み。
- 版を 26.09.17 に設定。

### フォルマントと音素セグメンテーション（Python 連携、2026-09-24）
C++ から `./.env` の Python を子プロセスで呼び、parselmouth（Praat）でフォルマント、
Montreal Forced Aligner（MFA）で単語/音素の区間を求めて画面に重ねる。
- **呼び出し方式**（`speech_tools.cpp` ↔ `python/speech_tools.py`）: 呼び出しごとに一時
  ディレクトリを作り、要求/応答を UTF-8 の JSON ファイルでやりとりする（コマンドライン経由の
  日本語が Windows の ANSI コードページで化けるのを避ける）。子の標準出力/エラーは同じ
  ディレクトリのログに回し、応答が無いときだけ末尾をエラーに添える。想定内の失敗は
  スクリプトが `ToolError` の文言を `error` に詰めて返す。
  - Windows は `CreateProcessW`（`CREATE_NO_WINDOW`）。ログのハンドルは起動の瞬間だけ継承可にする
    （同時に起動した別の子へ漏れて一時ディレクトリが消せなくなるのを防ぐ）。Linux は `posix_spawn`。
  - Python は環境変数 `MORPHALIGNER_PYTHON` → `.env/python.exe`（Linux は `.env/bin/python`）→
    `../.env/...` の順に探す。スクリプトは `python/` と `../python/`。
  - 終了時: 子は Job Object（Linux はプロセスグループ）に入れ、`main.cpp` がループ後に
    `terminate_speech_tools()` で孫の MFA ごと止める（止めないと future が子を待ち、閉じた後も
    数十秒固まる）。スクリプトの作業ファイルも C++ 側の一時ディレクトリ内に作らせるので残らない。
- **ジョブ**（`ui.cpp`）: `App::tool_jobs`（複数同時可、ui_job とは別枠）。完了時の適用処理で
  `Track::path` が変わっていたら結果を捨てる。実行中は `Track::formant_busy / align_busy`。
- **フォルマント**: Burg 法（time_step 5ms, 5 本推定, 最大フォルマント既定 5500Hz, 表示 F1–F4）。
  未定義フレームを除いて ERB に変換して保持し、スペクトログラムに `PlotScatter`（半径 1.5px,
  `NoFit`）で重ねる。色は F1 白 / F2 水色 / F3 桃 / F4 灰（viridis とアンカーの橙の両方から離す）。
- **音素セグメンテーション**: `mfa align_one`。japanese_mfa の辞書は 54 万行あり毎回全部読む
  ため 1 発話 3〜8 分かかっていた → **書き起こしの部分文字列になる語だけに絞った辞書**を渡して
  約 20 秒に短縮（sudachi の分割結果は書き起こしの部分文字列なので結果は同じ）。
  MFA は実行のたびにモデルを `<MFA_ROOT_DIR>/extracted_models` に展開し直し、同時実行で
  展開が衝突して壊れた状態が残るため、子の `MFA_ROOT_DIR` を呼び出しごとの一時ディレクトリに
  向け、音響モデルは zip のフルパスで渡す（base/target の同時実行・別の MFA と並行しても安全）。
- **表示**（`speech_view.cpp`）: 単語/音素の2段の独立プロット。base は上、target は下（ミニマップと
  同じく外側に置き、パネル間の対応線をまたがない）。時間軸は `SetupAxisLinks` で
  `Track::view_x0/x1` を共有してスペクトログラムと連動し、`BeginAlignedPlots` でプロット領域の
  左右端を揃える。無音（`<eps>`/`sil`）は塗らず、`spn`（辞書にない語）は橙。区間に収まらない
  ラベルは出さずホバーのツールチップで読む。結果が片方だけでも両方に枠を出して本体の高さを揃える。
- **UI**: 左パネルの「音声解析（Python）」。当初はトラックごとに書き起こしと「フォルマント」
  「音素アライン」ボタンを置いたが、同日の整理（下の「左パネルの整理」）で共通化・自動化した。
  設定（最大フォルマント、MFA のモデル名、Python の検出状況）は折りたたみの「設定」内。
- **IPA の字形**: MFA の日本語音素は ɕ ʑ と無声化の ̥（U+0325）を含み、Gen Interface JP に無い。
  OS のフォント（Windows: Segoe UI、Linux: DejaVu Sans）が見つかれば MergeMode で合成して補う
  （同梱はしない）。ImGui は結合文字を合成しないので ̥ は直後に小さく出る。
- 配布物に `python/` を追加（環境は同梱しない）。`.env` を `.gitignore` に追加。
- 検証: 検証用プログラム（スクラッチ）で、フォルマント（F1–F4 各 81 フレーム）、日本語を含む
  パス、base/target 同時アライン、空の書き起こしのエラー、終了処理（0.2 秒で戻り python/mfa が
  残らない）、一時ディレクトリが残らないことを確認。GUI はフォルマントの重ね描きを目視確認し、
  音素セグメンテーションのプロットはユーザーが動作確認済み。Linux 側のプロセス起動
  （posix_spawn）はビルド・動作とも未確認。

### 左パネルの整理（2026-09-24）
- Base/Target の「読み込む」「再生」を1行に。
- 「アンカー: n」の右に「(?)」（ホバーで操作説明の全文）と右寄せの「全消去」を置き、
  3 行あった操作説明をツールチップへ移した（`help_marker` / `right_aligned_button`）。
- **フォルマントは読み込み時に自動で推定**: `ensure_formants`（draw_root で毎フレーム）が
  `Track::formant_path != Track::path` を見て起動する。音声の読み込み・セッション読み込みの
  どちらの経路でも走り、起動時に formant_path を記録するので失敗しても再試行ループしない。
  最大フォルマントを変更・確定すると `invalidate_formants` で推定し直す。UI は表示の切替のみで、
  **表示は既定でオフ**（`App::show_formants = false`）。
- **書き起こしと音素アライメントを base/target で共通化**: `App::transcript` を1つだけ持ち、
  「音素アライメント」ボタンで読み込み済みの両方に MFA を走らせる（同時実行、約 20 秒）。
  セッション JSON は `transcript`（文字列、任意項目）で保存/復元する。

## MATLAB版との差分

※ **旧・自前実装についての比較**。現在は tcmorph（MATLAB 版の移植、丸め誤差レベルで一致を
検証済み）を使っているため、この節は履歴として残している。tcmorph と MATLAB の差分は
`tcmorph/docs/porting-notes.md`、2つのエンジンの違いは `tcmorph/docs/engines.md` を参照。

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
./bin/morphingAlignerCpp
```

- 実行時に `font/` と `licenses/` を相対パス（カレント起動と bin/ 起動の2候補）で開くので、
  リポジトリ直下（または配布物のルート）から起動する。
- Windows 機（開発機の1台）では `VCPKG_ROOT=C:\Users\skanno\.vcpkg`。既存の `build/` は
  Visual Studio ジェネレータで構成されており、出力は `bin/Release/`（CI は Ninja で `bin/` 直下）。

### Python ツールの環境（`./.env`）
フォルマント・音素セグメンテーションに使う。無くてもアプリは動く（読み込み時のフォルマント推定や音素アライメントが失敗をログに出す）。
```sh
micromamba create -p ./.env -c conda-forge python=3.13 montreal-forced-aligner
./.env/python -m pip install praat-parselmouth "sudachipy==0.6.11" "sudachidict-core==20260428"
# MFA のモデル（~/Documents/MFA/pretrained_models へ。環境を activate しない場合は
# .env/Library/bin 等を PATH に通して実行する。Linux は .env/bin/mfa）
mfa model download acoustic japanese_mfa
mfa model download dictionary japanese_mfa
```
- **sudachi の版は固定**: sudachipy 0.7 は MFA 付属の char.def（`NOOOVBOW2`）を読めず、
  sudachipy 0.6.11 は sudachidict-core 20260723 以降の辞書（ヘッダ版が新しい）を読めない。
  0.6.11 ＋ 20260428 で動作確認済み（MFA 3.4.2）。
- micromamba 環境を activate せずに python.exe を直接起動すると MFA が `libsndfile.dll` を
  読めずに落ちる。スクリプトが子プロセスの PATH に環境の `Library/bin` などを足して対処している。
- 別の場所の環境を使うときは環境変数 `MORPHALIGNER_PYTHON` に python のパスを入れる。

### リリース手順
1. `CMakeLists.txt` の `project(... VERSION x.y.z)` を更新してコミット。
2. `git tag vx.y.z && git push origin vx.y.z`（タグと VERSION が食い違うと CI が止まる）。
3. `release.yml` が両環境をビルドし、GitHub リリースを作成（同じタグの再実行は差し替え）。

## 既知の制約・メモ

- **Linux 開発機（Wayland）ではGUIのスクリーンショットが撮れない**（scrot/importは黒画面、grim未導入）。
  アプリ自体は正常動作。目視確認はユーザーが `./bin/morphingAlignerCpp` を起動して行う。
- ビルド/起動の検証は「数秒起動してstderrにエラーが出ないこと」で代替している。
- ヘッドレス検証用の素材は `.test/`（`hai1.wav`, `hai2.wav`, `test_session.json`）。`.gitignore` 対象なので
  マシン間では手で同期する（リポジトリには入らない）。
- base/target の fs 不一致は未対応（エラーで止める。リサンプリングなし）。
- macOS / Wayland ネイティブでは `glfwSetWindowIcon` が効かない（XWayland 上では効く）。
- `app.cpp` で `GL_CLAMP_TO_EDGE` を自前定義している（Windows の GL ヘッダに無いための応急処置、FIXME）。
- 書き起こしは base/target 共通なので、別の文を読んだ2音声には音素アライメントを使えない。
- MFA は1発話でも約 20 秒かかる（大半は MFA の起動とモデル展開）。書き起こしが音声と合わない、
  または辞書に無い語（`spn` になる）があると区間がずれる。フォルマント・音素セグメンテーションの
  結果は音声を読み直すと消え、セッションにも保存しない（書き起こしだけ保存する）。
- Windows の GUI 自動確認: 実マウスの操作は前面の別ウィンドウを誤操作しうるので使わない。
  `PostMessage` のクリックは実カーソルが窓外だと GLFW がカーソル離脱として扱い効かない。
  撮影だけなら `PrintWindow`（PW_RENDERFULLCONTENT）で隠れていても撮れる。

## 注意点（fragile）

- **周波数軸の手組み入力処理**（`ui.cpp` の `FIXME(freq-axis-input)` ブロック）:
  ホイールは X 専用ズームにするため Y 軸を `Lock` し、Y の表示範囲は `tr.y_min/y_max` に
  自前で保持して毎フレーム `SetupAxisLimits(Always)` で再適用している。ImPlot の軸状態を
  複製しているため壊れやすい（`IsPlotHovered` ゲートが2段プロット間のドラッグ跨ぎで誤作動、
  Lock 解除や Y 軸追加でデシンク等）。拡張時は自前処理を伸ばさず ImPlot に軸を任せる方向で。
  Ctrl+ホイールの Y ズームもこのブロックに乗っている。
- **周波数アンカーの並べ替え**: モーフィング時に base_f 順へ並べ替えるため、逆転したアンカーは
  tcmorph 単体と結果が変わる（UI に順序の概念がないための意図的な仕様）。

## 次にやること

- 再生の停止/一時停止・再生位置バー（現状は `play_oneshot` / `play_pcm` で頭から再生のみ）。
- アンカーの整列/ソートや、アンカー編集の Undo。
- base/target の fs 不一致への対応（リサンプリング）。
- 拡大時の見た目調整（`GL_LINEAR` ↔ `GL_NEAREST`）。
- Windows 版の実機確認（CI ビルド・.ico 埋め込み・フォント/ライセンスの表示）。
- 音素セグメンテーションのプロットの目視確認と調整、Linux での Python 連携の動作確認。
- 音素セグメンテーションの活用: 境界をスペクトログラムにも薄く重ねる、ホバー中の区間を
  スペクトログラム側で強調する、音素境界から時間アンカーを自動で打つ、など。
- フォルマント/セグメンテーション結果のセッション保存（再計算を省く）。
- 済: 時間アンカーの個別削除（右クリック）、対応線、周波数アンカー、モーフィング本体（tcmorph）、
  フォルマント表示・音素セグメンテーション（Python 連携）。
