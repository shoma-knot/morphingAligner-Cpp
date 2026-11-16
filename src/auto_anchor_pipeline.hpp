#pragma once

/// @file auto_anchor_pipeline.hpp
/// @brief 音声のパスと書き起こしから、アンカー自動生成までを一続きで行う（GUI に依存しない）。
///
/// GUI では ①環境の確認 ②フォルマント推定 ③移動平均 ④音素アライメント ⑤アンカー自動生成 を
/// それぞれジョブで走らせているが、ここではそれを順に（②と④は base / target を並行に）呼ぶ。
/// 計算の関数は GUI と同じもの（run_formants / smooth_formants / run_alignment /
/// generate_auto_anchors）を使うので、同じ設定なら GUI と同じアンカーになる。
/// batch サブコマンドの --auto-anchors で使う。ブロックするので、呼び出し側はそのつもりで。

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "auto_anchors.hpp"
#include "speech_tools.hpp"

// アンカー自動生成の設定（既定と範囲は GUI と同じ。SpeechState もこの範囲を使う）。
struct AutoAnchorSettings {
    std::string   transcript;                // MFA に渡す書き起こし（base / target 共通）
    int           divisions         = 2;     // 各音素の区間を何等分するか
    int           formant_window_ms = 50;    // フォルマントの移動平均の窓幅 [ms]
    FormantParams formant;                   // 最大フォルマントなど（base / target 共通）
    AlignParams   align;                     // MFA のモデル名

    static constexpr int    kWindowMsMin = 5, kWindowMsMax = 500;
    static constexpr int    kDivisionsMin = 1, kDivisionsMax = 10;
    static constexpr double kMaxFormantMin = 2000.0, kMaxFormantMax = 10000.0;
};

// Python のツールの呼び出し口。テストでは偽物に差し替える。
struct AutoAnchorTools {
    std::function<SpeechEnvStatus(const AlignParams&)>                                         check;
    std::function<Result<Formants>(const std::string&, const FormantParams&)>                  formants;
    std::function<Result<Segmentation>(const std::string&, const std::string&, const AlignParams&)> align;
    std::function<void()> terminate;    // 実行中のツールを止める（中止のとき）

    // 本物（speech_tools.hpp の run_check / run_formants / run_alignment / terminate_speech_tools）。
    static AutoAnchorTools real();
};

// 片側の音声。
struct AutoAnchorSource {
    std::string path;
    double      duration = 0;    // 音声の長さ [s]（WORLD の解析の MorphChannel::duration）
};

struct AutoAnchorPipelineResult {
    std::vector<Anchor>      anchors;
    std::vector<std::string> warnings;    // 生成はしたが注意が要る点
    std::string              error;       // 空なら成功
    bool                     canceled = false;
    int                      n_freq   = 0;    // 周波数アンカーの総数
    std::string              base_phones, target_phones;    // 無音を除いた音素列（空白区切り）

    bool ok() const { return error.empty(); }
};

// ①〜⑤を実行する。経過は log に1行ずつ渡す。cancel が立ったら実行中のツールを止めて
// canceled = true（error にも理由）で返す。
AutoAnchorPipelineResult build_auto_anchors(const AutoAnchorSource& base, const AutoAnchorSource& target,
                                            const AutoAnchorSettings& settings, const AutoAnchorTools& tools,
                                            const std::function<void(const std::string&)>& log,
                                            const std::atomic<bool>* cancel = nullptr);

// 生成の設定と結果の記録（書き出しの JSON や保存するセッションの "auto_anchors" の項目）を、
// JSON のオブジェクトの文字列にする。
std::string auto_anchor_record_json(const AutoAnchorSettings& settings, const AutoAnchorPipelineResult& r);
