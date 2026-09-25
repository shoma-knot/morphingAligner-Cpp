#pragma once

/// @file file_jobs.hpp
/// @brief ファイルダイアログを伴う操作（音声・セッションの読み込み/保存、WAV の書き出し）。
///
/// どれも App::jobs.ui で実行する（ダイアログはブロックするのでワーカーで開き、結果の反映は
/// メインスレッドで行う）。同時に1本に限るので、実行中は何もしない。ボタン側でも
/// app.jobs.ui.busy() のあいだは無効にしておくこと。

#include "anchor.hpp"    // Side

struct App;
struct LaunchOptions;

// 音声ファイルを選んで解析し、side のトラックに読み込む。
void launch_load_track_job(App& app, Side side);

// セッション（base/target のパス・書き起こし・アンカー）を JSON に保存する。
void launch_save_session_job(App& app);

// セッション（または tcmorph のアンカー JSON）を読み込む。音声の解析までワーカーで行う。
void launch_load_session_job(App& app);

// モーフィング結果の波形を WAV に保存する。
void launch_save_wav_job(App& app);

// コマンドライン引数の内容を反映する（起動直後に1回呼ぶ）。音声・セッションの読み込みは
// App::jobs.ui で行い、書き起こしはセッションの後に上書きする。--align は予約だけして、
// 読み込みと音声解析の環境の確認が済んでから実行する（speech_controller の ensure_pending_alignment）。
void apply_launch_options(App& app, const LaunchOptions& opts);
