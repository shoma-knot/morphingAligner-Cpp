#pragma once

/// @file morph_controller.hpp
/// @brief モーフィングタブの処理（解析チャンネルの用意、非同期のモーフィング、再生）。

#include <string>
#include <vector>

struct App;

// タブ表示時に base/target の解析チャンネルを Track から取り込む。音声が差し替わっていたら
// sp/ap のテクスチャと dB の範囲を作り直し、以前のモーフィング結果を無効にする。
void ensure_morph_channels(App& app);

// 非同期でモーフィングを開始する（実行中なら「最新条件で1回だけ再実行」を予約）。
// 結果を再生したいときは、呼ぶ前に MorphState::play_request を立てる。
void request_morph(App& app);

// 毎フレーム: 完了したモーフィングを回収し、結果とテクスチャを更新する（予約があれば再実行）。
void poll_morph_job(App& app);

// 波形をメモリから再生する（失敗はログに出す）。
void play_wave(App& app, const std::vector<double>& wave, int fs);

// 連続書き出しの既定値（読み込んである音声のファイル名と置き場所から作る）。
std::string batch_default_prefix(const App& app);
std::string batch_default_dir(const App& app);

// 設定（MorphState::batch）に従って連続書き出しを始める。App::jobs.ui のジョブなので、
// 実行中はタブの切り替えとファイル操作が止まる。ほかのジョブが動いていれば何もしない。
void start_batch_export(App& app);
