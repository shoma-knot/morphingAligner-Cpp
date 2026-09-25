#pragma once

/// @file speech_controller.hpp
/// @brief 音声解析（Python ツール）の実行と結果の反映、アンカー自動生成。
///
/// ツールは App::jobs.tools で実行する（他の操作を止めず、base/target を同時に走らせてよい）。
/// 結果は完了回収時にメインスレッドで Track に反映し、実行中に音声が差し替わっていたら捨てる。

struct App;

// 音声解析の環境をセットアップするスクリプトの名前（配布物のルートにある。OS で異なる）。
const char* install_script_name();

// 毎フレーム: 音声解析の環境が未確認なら、バックグラウンドで確認を始める（起動時と「再確認」時）。
void ensure_speech_env(App& app);

// 毎フレーム: 読み込まれている音声のフォルマントが未推定なら推定を始める（音声の読み込み・
// セッション読み込みのどちらの経路でも、パスが変われば自動で走る）。環境の確認が済むまで待つ。
void ensure_formants(App& app);

// 設定（最大フォルマントなど）を変えたときに、読み込み済みの音声で推定し直させる。
void invalidate_formants(App& app);

// 窓幅（SpeechState::formant_ma_ms）の変更後に、両トラックの移動平均を作り直す。
void update_formant_ma(App& app);

// 読み込まれている音声すべてで、共通の書き起こしによる音素アライメント（MFA）を始める。
void launch_alignment(App& app);

// アンカー自動生成を実行できるか（両トラックで音素アライメントとフォルマント推定が済んでいる）。
bool auto_anchors_ready(const App& app);

// 音素アライメントとフォルマントの移動平均からアンカーを作り、既存のアンカーと置き換える。
void run_auto_anchors(App& app);
