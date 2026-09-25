#pragma once

/// @file file_jobs.hpp
/// @brief ファイルダイアログを伴う操作（音声・セッションの読み込み/保存、WAV の書き出し）。
///
/// どれも App::jobs.ui で実行する（ダイアログはブロックするのでワーカーで開き、結果の反映は
/// メインスレッドで行う）。同時に1本に限るので、実行中は何もしない。ボタン側でも
/// app.jobs.ui.busy() のあいだは無効にしておくこと。

#include "anchor.hpp"    // Side

struct App;

// 音声ファイルを選んで解析し、side のトラックに読み込む。
void launch_load_track_job(App& app, Side side);

// セッション（base/target のパス・書き起こし・アンカー）を JSON に保存する。
void launch_save_session_job(App& app);

// セッション（または tcmorph のアンカー JSON）を読み込む。音声の解析までワーカーで行う。
void launch_load_session_job(App& app);

// モーフィング結果の波形を WAV に保存する。
void launch_save_wav_job(App& app);
