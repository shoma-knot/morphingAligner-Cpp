#pragma once

/// @file speech_tools.hpp
/// @brief Python の音声解析ツール（python/speech_tools.py）の呼び出しと結果の型。
///
/// フォルマント推定（parselmouth = Praat）と音素セグメンテーション（Montreal Forced
/// Aligner）を、./.env の Python を子プロセスとして起動して実行する。要求/応答は一時
/// ディレクトリの JSON ファイルでやりとりする（Windows でコマンドライン経由の日本語が
/// 化けるのを避けるため）。どの関数もブロックするので、必ずワーカースレッドで呼ぶこと。

#include <string>
#include <vector>

#include "result.hpp"

// フォルマント1本分の軌跡。未定義のフレームは除いてあり、erb は表示用（Y軸=ERB レート）。
struct FormantTrack {
    std::vector<double> t;      // 時刻 [s]
    std::vector<double> hz;     // 周波数 [Hz]
    std::vector<double> erb;    // 同・ERB レート
};

// フォルマント推定の結果（tracks[0] = F1, tracks[1] = F2, ...）。
struct Formants {
    std::vector<FormantTrack> tracks;
    bool empty() const { return tracks.empty(); }
};

// TextGrid の区間1つ。
struct SegInterval {
    double      start = 0, end = 0;    // [s]
    std::string label;                 // UTF-8（音素は IPA 記号）
};

// 区間ティア（MFA の出力では "words" と "phones"）。
struct SegTier {
    std::string              name;
    std::vector<SegInterval> intervals;
};

// 音素セグメンテーションの結果。
struct Segmentation {
    std::vector<SegTier> tiers;
    bool empty() const { return tiers.empty(); }

    // 音素ティア（MFA の "phones"。無ければ最後のティア）。無ければ nullptr。
    // MFA は単語ティア（"words"）も返すが、本アプリは音素ティアだけを使う。
    const SegTier* phones() const {
        for (const SegTier& t : tiers)
            if (t.name == "phones") return &t;
        return tiers.empty() ? nullptr : &tiers.back();
    }
};

// 無音を表すラベルか（MFA は音素ティアで "sil"、単語ティアで "<eps>" を出す）。
inline bool is_silence_label(const std::string& l) {
    return l.empty() || l == "<eps>" || l == "sil" || l == "sp";
}

// 音素ティアのうち、無音を除いた区間（時刻順。音素ティアが無ければ空）。
std::vector<const SegInterval*> spoken_phones(const Segmentation& seg);

// 区間のラベルを空白区切りでつなぐ（ログ用）。
std::string join_labels(const std::vector<const SegInterval*>& ivs);

// フォルマント推定のパラメータ（Praat の To Formant (burg) に対応）。
struct FormantParams {
    double max_formant_hz = 5500.0;    // 最大フォルマント（成人男性 5000 / 女性 5500 が目安）
    int    num_formants   = 5;         // 推定するフォルマント数
    int    num_tracks     = 4;         // 表示する本数（F1..F4）
};

// MFA のモデル指定（mfa model download で取得した名前、またはファイルパス）。
struct AlignParams {
    std::string acoustic_model = "japanese_mfa";
    std::string dictionary     = "japanese_mfa";
};

// 音声解析の環境チェック（speech_tools.py の check）の結果。
struct SpeechEnvStatus {
    bool                     ready = false;    // フォルマント推定・音素アライメントが使えるか
    std::vector<std::string> problems;         // 使えない理由（ready=false のとき）
    std::vector<std::string> warnings;         // 使えるが注意が要る点（パスの文字、古い環境など）
    std::string              summary;          // 版などの情報（ログ用）
};

// 使う Python と スクリプトの場所（見つからなければ空）。表示・診断用。
// 環境変数 MORPHALIGNER_PYTHON があればそれを優先し、なければ ./.env（と ../.env）を探す。
std::string find_python();
std::string find_speech_script();

// 実行中のツール（Python と、そこから起動された MFA）をすべて止め、以後の起動も断る。
// アプリ終了時、実行中のジョブの future を破棄する前に呼ぶ（呼ばないと子の終了まで待たされる）。
void terminate_speech_tools();

// 音声解析の環境を調べる（Python が見つからない・起動できない場合も ready=false で返す）。
SpeechEnvStatus run_check(const AlignParams& params);

// wav のフォルマントを推定する。
Result<Formants> run_formants(const std::string& wav, const FormantParams& params);

// wav を書き起こし text で強制アラインメントする。
Result<Segmentation> run_alignment(const std::string& wav, const std::string& text, const AlignParams& params);
