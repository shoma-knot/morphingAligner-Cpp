#pragma once

/// @file jobs.hpp
/// @brief ワーカースレッドで実行し、完了時にメインスレッドで結果を反映するジョブ。
///
/// ファイルダイアログ（tinyfd はダイアログを閉じるまでブロックする）、音声の解析、
/// Python ツール（フォルマント推定・MFA）など、描画を止めたくない処理に使う。
/// ワーカーは App に触れず、「メインスレッドで適用する処理」（JobApply）を返す。
/// GL（テクスチャ）や App の状態の変更は、必ずその適用処理の中で行うこと。

#include <functional>
#include <future>
#include <vector>

struct App;

// メインスレッドで App に結果を反映する処理（空なら何もしない）。
using JobApply = std::function<void(App&)>;
// ワーカーで実行する処理。終わったら JobApply を返す。
using JobWork = std::function<JobApply()>;

// 実行中のジョブの集まり。毎フレーム poll で完了したものを回収する。
class JobQueue {
public:
    // work をワーカーで開始する。work が例外を投げたらログに出し、代わりに on_error を
    // メインスレッドで呼ぶ（実行中フラグを下ろすなどの後始末に使う）。
    void launch(JobWork work, JobApply on_error = {});

    // 実行中のジョブが無ければ launch して true。あれば何もせず false（同時に1本に限る用途）。
    bool try_launch(JobWork work, JobApply on_error = {});

    // 完了したジョブの適用処理をメインスレッドで実行する。適用処理が投げた例外もログに出す。
    void poll(App& app);

    bool busy() const { return !jobs_.empty(); }

private:
    std::vector<std::future<JobApply>> jobs_;
};
