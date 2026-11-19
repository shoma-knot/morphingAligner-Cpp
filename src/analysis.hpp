#pragma once

/// @file analysis.hpp
/// @brief 音声ファイルの WORLD 解析（Harvest=F0, CheapTrick=スペクトル包絡, D4C=非周期性）。
///
/// 音声は読み込み時に1回だけ解析し、その結果（MorphChannel）をアライメントタブの表示と
/// モーフィングの両方で共有する（2026-09-25 まで表示用とモーフィング用で別々に解析していた）。

#include <memory>
#include <string>
#include <vector>

#include <tcmorph/generalized_tc_morphing.hpp>    // WorldParameter などの共通データ構造

#include "result.hpp"

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

// アライメントタブに重ねる F0 の線（MorphChannel::f0 から作る）。t は各フレームの時刻 [s]、erb は
// F0 の ERB レート（スペクトログラムの Y 軸の座標）。無声のフレーム（f0 = 0）は NaN にして、線を切る。
struct F0Curve {
    std::vector<double> t, erb;
    bool empty() const { return t.empty(); }
};

// チャンネルの F0（Harvest）から F0Curve を作る。
F0Curve make_f0_curve(const MorphChannel& c);

// 解析のオプション。既定は MATLAB 版（worldGUItools の worldHandler.mlapp）と同じ動作。
struct AnalysisOptions {
    // エンジン（wordTV2WmorphingEngineRev）に渡す f0_original の作り方。
    enum class F0Original {
        // worldHandler と同じ: 無声区間を埋めた途切れのない F0（continuous_f0）。
        WorldHandler,
        // 埋めない（空のまま）。エンジンは無声区間が 0 の source_parameter.f0 を代わりに使うので、無声のフレームは
        // 発話全体の最小の F0 で埋められ、有声との境目には低い F0 が混ざる（2026-11-19 までの動作）。
        Empty,
    };
    F0Original f0_original = F0Original::WorldHandler;
    // worldHandler と同じく、先頭フレームを必ず無声にする（Harvest の直後に f0(1) = 0、vuv(1) = 0）。
    // CheapTrick と D4C にも無声として渡る。
    bool first_frame_unvoiced = true;
    // Harvest の F0 の探索範囲 [Hz]。既定は worldHandler の既定（40〜800 Hz）。下限は有声・無声の判定にも効き、
    // 71 Hz（WORLD の既定、2026-11-19 までの値）だと発話の頭の息や無声の部分を有声と判定しやすい（実データの
    // 3 素材で、40 Hz なら MATLAB 版と最初の有声フレームと有声のフレーム数が一致した）。
    double f0_floor = 40.0;
    double f0_ceil  = 800.0;
    // CheapTrick と D4C の FFT 長。既定は worldHandler と同じ 4096（標本化周波数によらない。48 kHz で 2049 ビン）。
    // 0 なら WORLD の既定（標本化周波数と 71 Hz から決める。48 kHz で 2048。2026-11-19 までの値）。
    int fft_size = 4096;
};

// 2026-11-19 までの解析（f0_original は空、先頭フレームは Harvest のまま、F0 は 71〜800 Hz、FFT 長は WORLD の既定）。
AnalysisOptions legacy_analysis_options();

// 無声（f0 <= 0）のフレームを埋めた F0 を返す（worldHandler の setupWaveAxesView と同じ手順）。
//   - 最初の有声区間より前は最初の有声フレームの値、最後の有声区間より後は最後の有声フレームの値で埋める。
//   - 有声区間どうしの間は、前の区間の最後と次の区間の最初の値を Hz のまま直線でつなぐ（対数ではない）。
//   - 有声のフレームはそのまま。有声のフレームが1つも無ければ空を返す（worldHandler はエラーで止まる）。
Eigen::VectorXd continuous_f0(const Eigen::VectorXd& f0);

// 1音源を WORLD で解析して f0/sp/ap のチャンネルを作る。失敗時は error に理由（デコードの失敗など）。
Result<MorphChannel> analyze_channel(const std::string& path, const AnalysisOptions& opt = {});

// 音声ファイルの標本化周波数と長さ（WORLD の解析はしない）。
struct AudioInfo {
    int    fs       = 0;    // 標本化周波数 [Hz]
    double duration = 0;    // 長さ [s]（MorphChannel::duration と同じ計算）
};

// 音声ファイルの標本化周波数と長さを読む。デコードできなければ error に理由。
Result<AudioInfo> read_audio_info(const std::string& path);

// チャンネルから表示用の要約（dB の範囲など）を作る。
Spectrogram summarize_spectrogram(const MorphChannel& c);

// path をデコードして解析する（analyze_channel ＋ summarize_spectrogram）。
Result<AnalyzedAudio> analyze_file(const std::string& path, const AnalysisOptions& opt = {});
