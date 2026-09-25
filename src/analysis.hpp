#pragma once

/// @file analysis.hpp
/// @brief 音声ファイルの WORLD 解析（Harvest=F0, CheapTrick=スペクトル包絡, D4C=非周期性）。
///
/// 音声は読み込み時に1回だけ解析し、その結果（MorphChannel）をアライメントタブの表示と
/// モーフィングの両方で共有する（2026-09-25 まで表示用とモーフィング用で別々に解析していた）。

#include <memory>
#include <string>

#include <tcmorph/generalized_tc_morphing.hpp>    // WorldParameter などの共通データ構造

// 解析のフレーム周期 [ms]（WORLD の既定。tcmorph はフレーム間隔を入力から拾う）。
constexpr double kFramePeriodMs = 5.0;

// 1音源（base/target/morphed）の WORLD パラメータと表示用メタ情報。
// 実データ（f0/sp/ap）は world の中にある。sp/ap は tcmorph の向き＝(nbin, n_frames)
// の列優先で、1フレーム分が連続メモリに並ぶ。sp はパワースペクトル（dB ではない）。
struct MorphChannel {
    int    fs = 0, fft_size = 0, nbin = 0, n_frames = 0;
    double frame_period = 0;    // ms
    double duration     = 0;    // s

    tcmorph::WorldParameter world;

    bool empty() const { return n_frames == 0; }

    // 表示用アクセサ（world の中身への参照）。
    const Eigen::VectorXd& f0() const { return world.source_parameter.f0; }
    const Eigen::MatrixXd& sp() const { return world.spectrum_parameter.spectrogram; }
    const Eigen::MatrixXd& ap() const { return world.source_parameter.aperiodicity; }
};

// アライメントタブの表示に使う、スペクトル包絡の要約（実データは MorphChannel::sp）。
struct Spectrogram {
    int    num_frames = 0;    // 時間方向のフレーム数
    int    num_bins   = 0;    // 周波数ビン数（fft_size/2 + 1）
    int    fs         = 0;    // サンプリング周波数 [Hz]
    double duration   = 0;    // 信号の長さ [s]
    double db_min     = 0;    // スペクトル包絡の最小値 [dB]（カラースケールの範囲）
    double db_max     = 0;    // 同・最大値 [dB]

    bool empty() const { return num_frames == 0 || num_bins == 0; }
};

// 音声1本の解析結果。channel は immutable なので、ワーカー（モーフィング）とも共有してよい。
struct AnalyzedAudio {
    std::shared_ptr<const MorphChannel> channel;
    Spectrogram                         spec;
};

// 1音源を WORLD で解析して f0/sp/ap のチャンネルを作る。
// 失敗時は err に理由を入れ、empty() なチャンネルを返す。
MorphChannel analyze_channel(const std::string& path, std::string& err);

// チャンネルから表示用の要約（dB の範囲など）を作る。
Spectrogram summarize_spectrogram(const MorphChannel& c);

// path をデコードして解析する（analyze_channel ＋ summarize_spectrogram）。
// @throws std::runtime_error デコード・解析に失敗したとき。
AnalyzedAudio analyze_file(const std::string& path);
