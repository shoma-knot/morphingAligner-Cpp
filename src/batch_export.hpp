#pragma once

/// @file batch_export.hpp
/// @brief モーフィング率を等間隔に変えながら WAV を連続で書き出す。
///
/// 掃引する軸（全軸、または選んだ軸）を同じ値で連動させ、0（Base）→ 1（Target）を count 本に
/// 等分する。掃引しない軸は固定値のまま。刻み幅ではなく本数で指定するのは、実験用の刺激が
/// 「7 点連続体」のように本数で決まることが多いため。
/// GUI に依存しないので、呼び出しはワーカースレッドから行う（1本ごとに WORLD の合成が走る）。

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "analysis.hpp"    // MorphChannel
#include "anchor.hpp"
#include "morphing.hpp"    // MorphRates / kMorphAxes
#include "result.hpp"

// 連続書き出しの設定（UI のダイアログがそのまま持つ）。
struct BatchExportSettings {
    int         count = 11;             // 書き出す本数（率 0 → 1 を等分する点の数。両端を含む）
    // 掃引する軸（添字は kMorphAxes）。立っている軸をすべて同じ値で 0 → 1 に動かし（連動）、
    // 立っていない軸は fixed の値のままにする。既定は全軸（＝全軸一括の掃引）。
    bool        sweep[kMorphAxisCount] = { true, true, true, true, true };
    MorphRates  fixed;                  // 掃引しない軸の率（掃引する軸の値は使わない）
    std::string dir;                    // 出力先フォルダ（UTF-8）
    std::string prefix;                 // ファイル名の接頭辞
    bool        write_manifest = true;  // 率とアンカーを JSON に残すか

    // 本数の上限は、ファイル名に使う百分率（3桁）が隣どうしでぶつからない範囲。101 本なら
    // 刻みがちょうど 1% で、これを超えると 1% を割って同じ名前のファイルが2本できてしまう。
    static constexpr int kCountMin = 2, kCountMax = 101;
};

// ワーカーへ渡す入力。App には触れないので、必要なものだけをコピー（アンカー・設定）と
// 共有（解析済みチャンネルは immutable なので shared_ptr のまま渡してよい）で持つ。
struct BatchExportRequest {
    std::shared_ptr<const MorphChannel> base, target;
    std::vector<Anchor>                 anchors;
    // 書き出した JSON をそのままセッションとして読み直せるようにするための情報。
    std::string base_path, target_path, transcript;
    BatchExportSettings settings;
};

// 進捗と中止。UI スレッドとワーカーで共有する（JobQueue はどちらも持たないため）。
struct BatchExportProgress {
    std::atomic<int>  done { 0 };      // 書き出し済みの本数（ワーカーが進める）
    std::atomic<bool> cancel { false };    // 中止の指示（UI が立てる）
};

struct BatchExportResult {
    int                      written  = 0;    // 書き出せた本数
    bool                     canceled = false;
    std::string              manifest;    // 書き出した JSON のパス（書かなければ空）
    std::vector<std::string> warnings;    // モーフィングの警告（全本で同じなので1本目のもの）
    std::string              error;

    bool ok() const { return error.empty(); }
};

// count 本の率（0 → 1 を等分。両端を含む）。count が 2 未満なら空。
std::vector<double> batch_rates(int count);

// 全軸を掃引するか（ファイル名に軸の名前を入れるかどうかの判定に使う）。
bool batch_sweeps_all_axes(const BatchExportSettings& s);

// 掃引の値 rate のときの各軸の率（掃引する軸は rate、ほかは fixed の値）。
MorphRates batch_point_rates(const BatchExportSettings& s, double rate);

// 掃引の値 rate の WAV のファイル名。全軸なら "<接頭辞>_r050.wav"、選んだ軸だけなら
// "<接頭辞>_fo+sl_r050.wav" のように軸の名前を入れる（同じフォルダで別の軸を掃引しても
// 上書きし合わないように）。百分率を3桁のゼロ詰めにするのは、ファイル一覧で r010 が
// r100 より先に並ぶようにするため。
std::string batch_wav_name(const BatchExportSettings& s, double rate);

// 設定を残す JSON のファイル名（"<接頭辞>[_<軸>]_batch.json"）。
std::string batch_manifest_name(const BatchExportSettings& s);

// 設定を検査する（本数の範囲・掃引する軸・固定値の範囲・出力先の存在・接頭辞の文字）。
// 問題があれば理由を返す。
Status validate_batch_settings(const BatchExportSettings& s);

// 設定どおりに書き出したときに上書きすることになる既存ファイルの数（実行前の確認用）。
int count_existing_batch_files(const BatchExportSettings& s);

// 率を変えながらモーフィングして WAV を書き出す（ブロックするのでワーカーで呼ぶこと）。
// 1本書くごとに progress.done を進め、progress.cancel が立っていたらそこで止める。
// 途中で失敗したら中断して error に理由を入れる（アンカーの不備による失敗は率によらず
// 起きるので、1本目が通れば残りも通る）。
BatchExportResult run_batch_export(const BatchExportRequest& req, BatchExportProgress& progress);
