#pragma once

/// @file app.hpp
/// @brief アプリの状態（トラック・アンカー・音声解析・モーフィング・表示・ジョブ）と読み込みの反映。

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <implot.h>
#include <miniaudio_cpp/audio.hpp>

#include "analysis.hpp"
#include "anchor.hpp"
#include "anchors.hpp"    // EdgePoint
#include "gl_texture.hpp"
#include "jobs.hpp"
#include "morphing.hpp"    // MorphRates / MorphOutput
#include "speech_tools.hpp"    // Formants / Segmentation

// スペクトログラムのテクスチャと画面上のカラースケールで共有するカラーマップ。
constexpr ImPlotColormap kColormap = ImPlotColormap_Viridis;

// 読み込んだ音声1つ（base または target）と、その解析結果・表示用テクスチャ。
// GL テクスチャを持つのでコピー不可。
struct Track {
    std::string  name;       // "base" / "target"（ダイアログとラベルに出す）
    std::string  path;       // 読み込んだファイル（"" = 未読み込み）
    Spectrogram  spec;       // スペクトル包絡
    GlTexture    tex;        // spec を焼き込んだテクスチャ
    double       y_min = 0;  // 周波数軸の表示範囲 [ERB レート]。ズームで変わり、
    double       y_max = 0;  // 読み込み時に [0, ERB(fs/2)] に戻す

    // メインプロットの現在の表示範囲（ミニマップの枠に使う）。X=秒, Y=ERB レート。
    // view_x0/x1 はスペクトログラムと音素セグメンテーションの時間軸のリンク先も兼ねる
    // （ImPlot::SetupAxisLinks で両プロットが同じ値を読み書きする）。
    double view_x0 = 0, view_x1 = 0, view_y0 = 0, view_y1 = 0;

    // Python ツール（python/speech_tools.py）の結果。音声を読み直すと消える。
    Formants     formants;           // フォルマント（parselmouth。読み込み時に自動で推定）
    Formants     formants_ma;        // その移動平均（結果の受け取り時と窓幅の変更時に作り直す）
    Segmentation segmentation;       // 単語/音素の区間（Montreal Forced Aligner）
    std::string  formant_path;       // formants を推定した（または試みた）音声のパス
    bool         formant_busy = false;    // フォルマント推定を実行中
    bool         align_busy   = false;    // 音素セグメンテーションを実行中

    explicit Track(std::string n) : name(std::move(n)) {}

    bool loaded() const { return !path.empty(); }
};

// 音声解析（Python ツール）の環境の状態。起動時にバックグラウンドで確認し、Ready になるまで
// ツールは走らせない（環境が無いときに、音声を読み込むたびに失敗がログに積もるのを防ぐ）。
enum class SpeechEnv { Unknown, Checking, Ready, Unavailable };

// 音声解析（フォルマント・音素アライメント・アンカー自動生成）の設定と状態。
// 結果そのものは音声ごとなので Track が持つ。
struct SpeechState {
    SpeechEnv                env = SpeechEnv::Unknown;
    std::vector<std::string> env_problems;    // Unavailable の理由（ツールチップ用）

    FormantParams formant_params;
    AlignParams   align_params;
    std::string   transcript;    // MFA に渡す書き起こし（base/target 共通。同じ文を読んだ2音声を想定）

    bool show_formants     = false;    // フォルマントをスペクトログラムに重ねる
    bool show_formant_ma   = false;    // フォルマントの移動平均を線で重ねる
    int  formant_ma_ms     = 50;       // 移動平均の窓幅 [ms]
    bool show_segmentation = true;     // 音素セグメンテーションのプロットを出す
    int  auto_anchor_divisions = 2;    // アンカー自動生成で各音素の区間を何等分するか
};

// モーフィングタブの設定と状態。
struct MorphState {
    MorphRates rates;               // 軸ごとの率（0=base, 1=target）
    bool       link     = true;     // 全軸を一括操作するか
    bool       realtime = true;     // スライダー操作中も逐次再合成するか（OFF=離した時のみ）
    bool       autoplay = false;    // スライダーのつまみを離したら自動で再生するか

    // base/target の解析チャンネル（添字は side_index。タブ表示時に解析、パス変更で再解析）。
    // 非同期ジョブと安全に共有するため immutable な shared_ptr で保持する（失敗時は nullptr）。
    std::shared_ptr<const MorphChannel> ch[2];
    std::string                         ch_path[2];    // 解析済みチャンネルの元パス
    MorphOutput                         out;           // 再合成の結果（morphed の f0/sp/ap＋wave）

    // 非同期モーフィングジョブ（ワーカーで morphing_channels を実行。GL への反映は
    // 完了回収時にメインスレッドで行う）。実行中の再要求は pending に畳んで最新条件で1回だけ再実行。
    //
    // 再生の予約は「要求」と「実行中のジョブ」で分けて持つ。実行中に再要求が来ると
    // pending に畳まれるため、1つの旗を使い回すと古い率の結果が再生されてしまう。
    //   play_request … 次に開始するモーフィングの結果を再生する
    //   job_play     … 実行中のジョブが完了したら再生する（開始時に確定）
    std::future<MorphOutput>              job;
    bool                                  job_running  = false;
    bool                                  job_pending  = false;    // 実行中に来た再要求
    bool                                  play_request = false;
    bool                                  job_play     = false;
    int                                   epoch = 0, job_epoch = 0;    // base/target の世代
    std::chrono::steady_clock::time_point job_t0;                      // 計測用
    // 前回ログに出した警告。アンカーが同じなら毎回同じ内容になるので、変わったときだけ出す。
    std::vector<std::string> last_warnings;

    // sp/ap ヒートマップ用テクスチャ（0=base, 1=morphed, 2=target）。
    GlTexture tex_sp[3];
    GlTexture tex_ap[3];
    double    db_min = 0, db_max = 0;    // sp 共通の dB レンジ

    // base/target の解析チャンネル（未解析・失敗なら nullptr）。
    const MorphChannel* channel(Side s) const { return ch[side_index(s)].get(); }
};

// 画面の表示状態（フレームをまたいで持つもの）。
struct ViewState {
    bool     show_minimap = false;      // スペクトログラムのミニマップ表示
    bool     log_open     = true;       // 下部ログ領域の展開状態
    ImFont*  mono_font    = nullptr;    // 等幅フォント（ライセンス表示用、null なら既定）

    // アンカーの番号と対応線は、カーソル近傍の時間アンカー1本ぶんだけ描く
    // （24本まで増えると全点にラベルが出て読めないため）。色や太さでの強調は
    // カーソル移動のたびに周囲が明滅してうるさかったので行わない。
    // base/target の2パネルで共有する必要があるが、base を描く時点では target 側の
    // ホバーが未確定なので、今フレームのホバーを hover_anchor に集めて次フレームの
    // active_anchor に回す（ミニマップの表示枠と同じ1フレーム遅延）。
    int active_anchor = -1;    // 番号と対応線を出す時間アンカー（-1 = なし）
    int hover_anchor  = -1;    // 今フレームにホバーされたもの（次フレームの active）

    // パネル間の対応線の端点（添字は side_index。スペクトログラムを描くたびに作り直す）。
    std::vector<EdgePoint> edges[2];

    // ライセンス表示タブ。
    int         license_selected = 0;
    int         license_loaded   = -1;    // text に読み込み済みの選択（変わったら読み直す）
    std::string license_text;
};

// 実行中の非同期ジョブ（モーフィングは MorphState::job で別に持つ）。
struct Jobs {
    // ファイルダイアログや音声の解析など。同時に1本に限る（try_launch）。実行中はタブの
    // 切り替えと、ダイアログを開くボタンを無効にする。
    JobQueue ui;
    // Python ツール（フォルマント推定・MFA）と環境チェック。他の操作を止めず、同時に走らせてよい。
    JobQueue tools;
};

// フレームをまたいで共有するアプリの状態。
struct App {
    ma::engine          engine;    // 音声の出力デバイス（base/target で共有）
    Track               base { "base" };
    Track               target { "target" };
    std::vector<Anchor> anchors;    // base と target の対応

    SpeechState speech;
    MorphState  morph;
    ViewState   view;
    // ジョブは最後に置く（破棄は逆順なので、実行中のジョブの終了を待ってから他の状態を破棄する）。
    Jobs jobs;

    Track&       track(Side s) { return s == Side::Base ? base : target; }
    const Track& track(Side s) const { return s == Side::Base ? base : target; }
};

// base/target の sp/ap テクスチャと共通 dB レンジを作り直す（morphed テクスチャも
// 新レンジで作り直す）。morphed は base/target の log 補間なので必ずレンジ内に収まる。
void rebuild_morph_bt_textures(MorphState& m);

// morphed の sp/ap テクスチャだけを作り直す（レンジは計算済みのものを使用）。
void rebuild_morphed_texture(MorphState& m);

// 解析済みの Spectrogram を Track に反映する（テクスチャ生成・表示状態リセット・ログ）。
// GL を使うため必ずメインスレッドで呼ぶこと（解析はワーカーで analyze_file を使う）。
void apply_track(Track& tr, const std::string& path, Spectrogram&& spec);
