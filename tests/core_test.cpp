// コアのライブラリ（GUI に依存しない部分）の単体テスト。
//
// 対象: 周波数尺度の変換、アンカーの Side による参照、音声の解析（合成音の WAV を一時ディレクトリに作る）、コマンドライン引数、フォルマントの移動平均、モーフィング用のアンカー整形、
// アンカー自動生成、OT による周波数アンカー、包絡の凹凸を戻す処理と OT での補間、連続書き出し、セッション JSON の読み書き。Python は使わないので、
// どこで実行してもよい（CI の build.yml がビルド直後に各 OS で実行する）。
//
// ビルド: cmake -DMORPHALIGNER_BUILD_TESTS=ON ... → bin/core_test

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "analysis.hpp"
#include "anchor.hpp"
#include "auto_anchor_pipeline.hpp"
#include "auto_anchors.hpp"
#include "cli_commands.hpp"
#include "batch_export.hpp"
#include "formant_smoothing.hpp"
#include "freqscale.hpp"
#include "launch_options.hpp"
#include "morphing.hpp"
#include "ot_anchors.hpp"
#include "session_io.hpp"
#include "envelope_transport.hpp"

#include <nlohmann/json.hpp>

namespace {

int g_failures = 0;

void expect(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? "OK" : "NG", what.c_str());
    if (!ok) ++g_failures;
}

bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

// 失敗していて、その理由が prefix で始まるか。
template <class T>
bool fails_with(const Result<T>& r, const std::string& prefix) {
    return !r.ok() && r.error.rfind(prefix, 0) == 0;
}

// ── 周波数尺度 ──────────────────────────────────────────────
void test_freqscale() {
    bool ok = true;
    for (double hz : { 0.0, 100.0, 1000.0, 8000.0, 22050.0 })
        ok &= near(freqscale::erb_to_hz(freqscale::hz_to_erb(hz)), hz, 1e-6);
    expect(ok, "freqscale: Hz → ERB → Hz で元に戻る");
    expect(near(freqscale::hz_to_erb(0.0), 0.0), "freqscale: 0 Hz は ERB 0");
}

// ── アンカーの Side による参照 ──────────────────────────────
void test_anchor_side() {
    Anchor a { 0.1, 0.2, { { 500, 600 } } };
    a.time(Side::Target) += 0.1;
    a.freqs[0].freq(Side::Base) = 550;
    const Anchor& c = a;
    expect(near(c.time(Side::Base), 0.1) && near(a.target_t, 0.3) && near(a.freqs[0].base_f, 550)
             && near(c.freqs[0].freq(Side::Target), 600),
           "anchor: time(Side) / freq(Side) が base / target の値を指す");
    expect(side_index(Side::Base) == 0 && side_index(Side::Target) == 1 && kSides[1] == Side::Target,
           "anchor: side_index と kSides");
}

// ── フォルマントの移動平均 ──────────────────────────────────
FormantTrack make_track(const std::vector<double>& t, const std::vector<double>& hz) {
    FormantTrack tr;
    tr.t  = t;
    tr.hz = hz;
    for (double h : hz) tr.erb.push_back(freqscale::hz_to_erb(h));
    return tr;
}

void test_smooth_formants() {
    // 一定値はそのまま。
    {
        Formants f;
        f.tracks.push_back(make_track({ 0.00, 0.01, 0.02, 0.03 }, { 500, 500, 500, 500 }));
        const Formants s = smooth_formants(f, 0.05);
        bool           ok = s.tracks.size() == 1 && s.tracks[0].t.size() == 4;
        for (double h : s.tracks[0].hz) ok &= near(h, 500.0);
        expect(ok, "smooth_formants: 一定値は変わらない");
    }
    // 窓の中の平均（幅 0.02 s → 前後 1 点ずつ。端は欠けた分だけ点数が減る）。
    {
        Formants f;
        f.tracks.push_back(make_track({ 0.00, 0.01, 0.02 }, { 100, 200, 600 }));
        const Formants s = smooth_formants(f, 0.0201);
        const auto&    h = s.tracks[0].hz;
        expect(h.size() == 3 && near(h[0], 150.0) && near(h[1], 300.0) && near(h[2], 400.0),
               "smooth_formants: 時刻を中心とする窓で平均する");
        expect(near(s.tracks[0].erb[1], freqscale::hz_to_erb(300.0)), "smooth_formants: erb は平均の Hz から作る");
    }
    // 推定できなかったフレームで区切れた区間はまたがず、境目に NaN を1点挟む。
    {
        Formants f;
        f.tracks.push_back(make_track({ 0.00, 0.01, 0.10, 0.11 }, { 100, 100, 900, 900 }));
        const Formants s = smooth_formants(f, 1.0);
        const auto&    h = s.tracks[0].hz;
        expect(h.size() == 5 && near(h[0], 100.0) && near(h[1], 100.0) && std::isnan(h[2]) && near(h[3], 900.0)
                 && near(h[4], 900.0),
               "smooth_formants: 途切れた区間をまたがず、境目に NaN を挟む");
    }
    expect(smooth_formants(Formants {}, 0.05).empty(), "smooth_formants: 空の入力は空");
}

// ── モーフィング用のアンカー整形 ────────────────────────────
void test_build_anchor_matrices() {
    // 範囲外（0 以下・終端以上）は落とす。
    {
        const std::vector<Anchor> a = { { 0.0, 0.5, {} }, { 0.5, 0.5, {} }, { 1.5, 0.7, {} }, { 0.8, 2.0, {} } };
        const AnchorMatrices      m = build_anchor_matrices(a, 1.0, 1.0, 8000.0);
        expect(m.t_ref.size() == 1 && near(m.t_ref[0], 0.5) && m.dropped_time == 3,
               "build_anchor_matrices: 解析範囲の外の時間アンカーを落とす");
    }
    // base_t の順に並べ、target_t が逆転する（線が交差する）ものは残せる本数が最大になるよう落とす。
    {
        // base 順: (0.1,0.1) (0.2,0.9) (0.3,0.3) (0.4,0.4) → 0.9 の1本を落とすのが最大。
        const std::vector<Anchor> a = { { 0.4, 0.4, {} }, { 0.2, 0.9, {} }, { 0.1, 0.1, {} }, { 0.3, 0.3, {} } };
        const AnchorMatrices      m = build_anchor_matrices(a, 1.0, 1.0, 8000.0);
        bool ok = m.t_ref.size() == 3 && m.dropped_time == 1;
        if (ok)
            ok = near(m.t_ref[0], 0.1) && near(m.t_ref[1], 0.3) && near(m.t_ref[2], 0.4) && near(m.t_tgt[1], 0.3);
        expect(ok, "build_anchor_matrices: 交差する時間アンカーを最小限だけ落とす");
    }
    // 周波数アンカーは範囲外を落とし、base_f の昇順に 0 詰めで並べる。
    {
        const std::vector<Anchor> a = {
            { 0.2, 0.2, { { 2000, 2100 }, { 500, 600 }, { 0.5, 100 } } },
            { 0.6, 0.6, { { 1000, 1100 } } },
        };
        const AnchorMatrices m = build_anchor_matrices(a, 1.0, 1.0, 8000.0);
        const bool ok = m.tf_ref.rows() == 2 && m.tf_ref.cols() == 2 && near(m.tf_ref(0, 0), 500)
                     && near(m.tf_ref(1, 0), 2000) && near(m.tf_tgt(0, 0), 600) && near(m.tf_ref(0, 1), 1000)
                     && near(m.tf_ref(1, 1), 0) && m.dropped_freq == 1;
        expect(ok, "build_anchor_matrices: 周波数アンカーを並べ替えて 0 詰めにする");
    }
}

// ── アンカー自動生成 ────────────────────────────────────────
Segmentation make_phones(const std::vector<SegInterval>& iv) {
    Segmentation s;
    s.tiers.push_back(SegTier { "words", {} });
    s.tiers.push_back(SegTier { "phones", iv });
    return s;
}

void test_auto_anchors() {
    // base: sil a k a sil / target: 時間を 2 倍に伸ばしたもの。
    const Segmentation sb = make_phones({ { 0.0, 0.1, "sil" }, { 0.1, 0.2, "a" }, { 0.2, 0.3, "k" },
                                          { 0.3, 0.5, "a" }, { 0.5, 0.6, "sil" } });
    const Segmentation st = make_phones({ { 0.0, 0.2, "sil" }, { 0.2, 0.4, "a" }, { 0.4, 0.6, "k" },
                                          { 0.6, 1.0, "a" }, { 1.0, 1.2, "sil" } });
    // F1 だけ。base は 500 Hz、target は 700 Hz で一定。
    Formants fb, ft;
    fb.tracks.push_back(make_track({ 0.0, 0.6 }, { 500, 500 }));
    ft.tracks.push_back(make_track({ 0.0, 1.2 }, { 700, 700 }));
    const AutoAnchorInput in_b { sb, fb, 0.6 };
    const AutoAnchorInput in_t { st, ft, 1.2 };

    {
        const AutoAnchorResult r = generate_auto_anchors(in_b, in_t, 1);
        // 各音素の始まり 3 本と、最後の音素の終わり 1 本。
        bool ok = r.ok() && r.anchors.size() == 4 && r.warnings.empty();
        if (ok)
            ok = near(r.anchors[0].base_t, 0.1) && near(r.anchors[0].target_t, 0.2) && near(r.anchors[3].base_t, 0.5)
              && near(r.anchors[3].target_t, 1.0);
        expect(ok, "generate_auto_anchors: 音素境界に時間アンカーを打つ");
        ok = r.ok() && r.n_freq == 4 && r.anchors[1].freqs.size() == 1 && near(r.anchors[1].freqs[0].base_f, 500)
          && near(r.anchors[1].freqs[0].target_f, 700);
        expect(ok, "generate_auto_anchors: 時間アンカーの時刻のフォルマントに周波数アンカーを打つ");
    }
    {
        const AutoAnchorResult r = generate_auto_anchors(in_b, in_t, 2);
        expect(r.ok() && r.anchors.size() == 7 && near(r.anchors[1].base_t, 0.15) && near(r.anchors[1].target_t, 0.3),
               "generate_auto_anchors: 分割数 2 で各音素の中点にも打つ");
    }
    {
        const Segmentation    sx = make_phones({ { 0.1, 0.2, "a" }, { 0.2, 0.3, "k" } });
        const AutoAnchorInput in_x { sx, fb, 0.6 };
        const AutoAnchorResult r = generate_auto_anchors(in_b, in_x, 1);
        expect(!r.ok() && r.anchors.empty(), "generate_auto_anchors: 音素の数が違えば失敗する");
    }
    {
        const Segmentation sx = make_phones({ { 0.0, 0.1, "sil" }, { 0.1, 0.2, "a" }, { 0.2, 0.3, "t" },
                                              { 0.3, 0.5, "a" }, { 0.5, 0.6, "sil" } });
        const AutoAnchorInput  in_x { sx, fb, 0.6 };
        const AutoAnchorResult r = generate_auto_anchors(in_x, in_t, 1);
        expect(r.ok() && r.warnings.size() == 1, "generate_auto_anchors: ラベルが違えば警告を出して続ける");
    }
}

// ── 音声の解析（表示とモーフィングで共有する1回の解析） ─────────
void test_analysis() {
    // 0.5 秒の合成音（150 Hz の鋸歯状波に近い倍音列）を WAV にして解析する。
    constexpr int       fs = 16000;
    std::vector<double> x(fs / 2);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        for (int h = 1; h <= 10; ++h) x[i] += 0.3 / h * std::sin(2.0 * 3.14159265358979 * 150.0 * h * t);
    }
    const std::filesystem::path wav = std::filesystem::temp_directory_path() / "morphaligner_core_test.wav";
    expect(write_wav(wav.string(), x, fs).ok(), "analysis: テスト用の WAV を書ける");

    const Result<AnalyzedAudio> r = analyze_file(wav.string());
    std::error_code             ec;
    std::filesystem::remove(wav, ec);
    expect(r.ok() && r.value.channel, "analysis: analyze_file が成功する" + (r.ok() ? "" : "（" + r.error + "）"));
    if (!r.ok() || !r.value.channel) return;
    const AnalyzedAudio& a = r.value;

    const MorphChannel& c = *a.channel;
    expect(c.fs == fs && near(c.duration, 0.5, 1e-3) && c.n_frames > 0 && c.sp().cols() == c.n_frames
             && c.ap().cols() == c.n_frames && c.f0().size() == c.n_frames,
           "analysis: f0 / sp / ap がフレーム数ぶん揃う");
    expect(a.spec.fs == c.fs && a.spec.num_frames == c.n_frames && a.spec.num_bins == c.nbin
             && near(a.spec.duration, c.duration) && a.spec.db_min < a.spec.db_max,
           "analysis: 表示用の要約がチャンネルと一致する");
    expect(near(a.spec.db_max, 10.0 * std::log10(c.sp().maxCoeff())), "analysis: dB の範囲は sp から求める");

    // F0 の線: フレームごとに時刻と ERB レート、無声のフレームは NaN（線の切れ目）。
    {
        const F0Curve f = make_f0_curve(c);
        expect(static_cast<int>(f.t.size()) == c.n_frames && f.erb.size() == f.t.size()
                 && near(f.t[1] - f.t[0], c.frame_period / 1000.0),
               "make_f0_curve: フレームごとに時刻を持つ");
        int  voiced = 0;
        bool ok     = true;
        for (std::size_t i = 0; i < f.t.size(); ++i) {
            const double hz = c.f0()[static_cast<Eigen::Index>(i)];
            if (hz > 0.0) {
                ++voiced;
                ok &= near(f.erb[i], freqscale::hz_to_erb(hz));
            } else {
                ok &= std::isnan(f.erb[i]);
            }
        }
        expect(ok && voiced > 0, "make_f0_curve: 有声は F0 の ERB レート、無声は NaN");
        const std::size_t mid = f.t.size() / 2;
        expect(near(freqscale::erb_to_hz(f.erb[mid]), 150.0, 5.0), "make_f0_curve: 150 Hz の合成音で約 150 Hz");
        expect(make_f0_curve(MorphChannel {}).empty(), "make_f0_curve: 空のチャンネルなら空");
    }

    // 同じチャンネルどうしのモーフィング（アンカーなし）が最後まで通る。
    const MorphOutput o = morphing_channels(c, c, {}, MorphRates::uniform(0.5));
    expect(o.ok() && o.fs == fs && !o.wave.empty() && std::fabs(static_cast<double>(o.wave.size()) - c.duration * fs) < fs * 0.02,
           "analysis: 解析結果をそのままモーフィングに使える");

    // ASCII 以外を含むパス（UTF-8）。アプリ内のパスは UTF-8 で持つ（ファイルダイアログもコマンドライン
    // 引数も UTF-8）。書いたファイルが正しい名前でできていること、正しい名前で置いたファイルを読めることを
    // 別々に確かめる（同じ関数で書いて読むだけだと、名前が化けていても往復では通ってしまう）。
    {
        const std::filesystem::path dir   = std::filesystem::temp_directory_path();
        const std::filesystem::path named = dir / std::filesystem::u8path(u8"morphaligner_テスト音声.wav");
        std::filesystem::remove(named, ec);

        const Status ws = write_wav(named.u8string(), x, fs);
        expect(ws.ok() && std::filesystem::exists(named), "analysis: 日本語を含むパス（UTF-8）に WAV を書ける"
                                                            + (ws.ok() ? "" : "（" + ws.error + "）"));
        std::filesystem::remove(named, ec);

        const std::filesystem::path ascii = dir / "morphaligner_ascii_copy.wav";
        const Status                ws2   = write_wav(ascii.string(), x, fs);
        std::filesystem::copy_file(ascii, named, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(ascii, ec);
        const Result<AnalyzedAudio> ur = analyze_file(named.u8string());
        std::filesystem::remove(named, ec);
        expect(ws2.ok() && ur.ok() && ur.value.channel && ur.value.channel->n_frames == c.n_frames,
               "analysis: 日本語を含むパス（UTF-8）の WAV を読める" + (ur.ok() ? "" : "（" + ur.error + "）"));
    }

    const Result<AnalyzedAudio> bad =
      analyze_file((std::filesystem::temp_directory_path() / "morphaligner_no_such_file.wav").string());
    expect(!bad.ok() && !bad.error.empty() && !bad.value.channel, "analysis: 開けないファイルは失敗（理由つき）");
}

// ── コマンドライン引数 ──────────────────────────────────────
void test_launch_options() {
    using V = std::vector<std::string>;
    {
        const Result<LaunchOptions> r = parse_launch_options({});
        expect(r.ok() && r.value.command == LaunchCommand::Gui && !r.value.has_files() && !r.value.align,
               "launch: 引数なしは何も読み込まずに gui");
    }
    {
        const Result<LaunchOptions> r =
          parse_launch_options(V { "gui", "--base", "a.wav", "--target=b.wav", "--transcript", "あいうえお", "--align" });
        const LaunchOptions& o = r.value;
        expect(r.ok() && o.command == LaunchCommand::Gui && o.base == "a.wav" && o.target == "b.wav" && o.has_transcript
                 && o.transcript == "あいうえお" && o.align && o.session.empty(),
               "launch: 「--name 値」と「--name=値」の両方を読める");
    }
    {
        const Result<LaunchOptions> r = parse_launch_options(V { "GUI", "--session", "s.json", "--align" });
        expect(r.ok() && r.value.command == LaunchCommand::Gui && r.value.session == "s.json" && r.value.align
                 && !r.value.has_transcript,
               "launch: サブコマンドは大文字でもよい。--session があれば --align に --transcript は要らない");
    }
    {
        const Result<LaunchOptions> r = parse_launch_options(V { "gui", "--transcript=" });
        expect(r.ok() && r.value.has_transcript && r.value.transcript.empty(), "launch: 空の書き起こしも指定できる");
    }
    {
        const Result<LaunchOptions> r = parse_launch_options(V { "--help", "--nope" });
        expect(r.ok() && r.value.help && r.value.command == LaunchCommand::None, "launch: サブコマンドなしの --help は全体の説明");
    }
    expect(parse_launch_options(V { "--version" }).value.version, "launch: サブコマンドなしの --version");
    {
        const Result<LaunchOptions> r = parse_launch_options(V { "export", "--nope", "-h" });
        expect(r.ok() && r.value.help && r.value.command == LaunchCommand::Export,
               "launch: サブコマンドの --help はほかの誤りより優先");
    }
    expect(fails_with(parse_launch_options(V { "--base", "a.wav" }), "サブコマンドがありません"),
           "launch: 引数があるのにサブコマンドが無ければ誤り");
    expect(fails_with(parse_launch_options(V { "a.wav" }), "知らないサブコマンドです: a.wav"), "launch: 知らないサブコマンド");
    expect(launch_command_of(V { "Export", "x" }) == LaunchCommand::Export
             && launch_command_of(V { "auto-anchors" }) == LaunchCommand::AutoAnchors
             && launch_command_of(V { "convert" }) == LaunchCommand::Convert && launch_command_of(V { "x" }) == LaunchCommand::None
             && launch_command_of(V { "batch" }) == LaunchCommand::None,
           "launch: 最初の引数からサブコマンドを判定");
    expect(fails_with(parse_launch_options(V { "gui", "a.wav" }), "知らない引数です: a.wav"),
           "launch: オプション名の無い引数は受け付けない");
    expect(fails_with(parse_launch_options(V { "gui", "--bass", "a.wav" }), "知らない引数です: --bass"), "launch: 知らないオプション");
    expect(fails_with(parse_launch_options(V { "gui", "--base" }), "--base に値がありません"), "launch: 値の無いオプション");
    expect(fails_with(parse_launch_options(V { "gui", "--base=" }), "--base の値が空です"), "launch: 空のパス");
    expect(fails_with(parse_launch_options(V { "gui", "--base", "a", "--base", "b" }), "--base が2回指定されています"),
           "launch: 同じオプションの重複");
    expect(fails_with(parse_launch_options(V { "gui", "--align=1", "--base", "a" }), "知らない引数です: --align=1"),
           "launch: 値を取らないオプションに = は付けられない");
    expect(fails_with(parse_launch_options(V { "gui", "--transcript", "x", "--align" }), "--align には --base"),
           "launch: 音声なしの --align");
    expect(fails_with(parse_launch_options(V { "gui", "--base", "a.wav", "--align" }), "--align には --transcript"),
           "launch: 書き起こしなしの --align");
    expect(fails_with(parse_launch_options(V { "gui", "--count", "7" }), "--count は gui では使えません"),
           "launch: export のオプションは gui で使えない");
    expect(fails_with(parse_launch_options(V { "batch", "--session", "s.json", "--count", "3", "--out", "o" }),
                      "batch サブコマンドはなくなりました"),
           "launch: batch はなくなったことを知らせる");

    // export
    {
        const Result<LaunchOptions> r = parse_launch_options(V { "export", "s.json", "--count=7", "--out", "o", "--prefix",
                                                                 "p", "--sweep", "fo,SL", "--fixed", "tx=0.5,fx=1,ap=0",
                                                                 "--no-manifest", "--overwrite" });
        const ExportCliOptions& b = r.value.exp;
        expect(r.ok() && r.value.command == LaunchCommand::Export && r.value.input == "s.json" && b.count == 7
                 && r.value.out == "o" && b.prefix == "p" && !b.sweep[0] && !b.sweep[1] && b.sweep[2] && b.sweep[3]
                 && !b.sweep[4] && near(b.fixed[0], 0.5) && near(b.fixed[1], 1.0) && near(b.fixed[4], 0.0) && !b.manifest
                 && r.value.overwrite,
               "launch: export の設定を読める（入力はオプション名なし）" + (r.ok() ? "" : "（" + r.error + "）"));
    }
    {
        const Result<LaunchOptions> r = parse_launch_options(V { "export", "--count", "2", "--out", "o", "s.json" });
        const ExportCliOptions&     b = r.value.exp;
        expect(r.ok() && r.value.input == "s.json" && b.manifest && !r.value.overwrite && b.prefix.empty() && b.sweep[0]
                 && b.sweep[4],
               "launch: export の既定は全軸・JSON あり・上書きしない。入力はどこに書いてもよい");
    }
    const auto ex = [&](V extra) {
        V v { "export", "s.json" };
        v.insert(v.end(), extra.begin(), extra.end());
        return parse_launch_options(v);
    };
    expect(fails_with(ex({ "--out", "o" }), "export には --count"), "launch: export は --count が必須");
    expect(fails_with(ex({ "--count", "3" }), "export には --out"), "launch: export は --out が必須");
    expect(fails_with(parse_launch_options(V { "export", "--count", "3", "--out", "o" }), "export には入力の JSON が要ります"),
           "launch: export は入力が必須");
    expect(fails_with(ex({ "t.json", "--count", "3", "--out", "o" }), "入力の JSON は1つだけ"), "launch: 入力が2つ");
    expect(fails_with(ex({ "--count", "1", "--out", "o" }), "--count は 2〜101"), "launch: 本数が少なすぎる");
    expect(fails_with(ex({ "--count", "102", "--out", "o" }), "--count は 2〜101"), "launch: 本数が多すぎる");
    expect(fails_with(ex({ "--count", "3x", "--out", "o" }), "--count は 2〜101"), "launch: 本数が整数でない");
    expect(fails_with(ex({ "--count", "3", "--out", "o", "--sweep", "fo,f0" }), "--sweep に知らない軸があります: f0"),
           "launch: 知らない軸");
    expect(fails_with(ex({ "--count", "3", "--out", "o", "--fixed", "tx=0.5" }), "--fixed の tx は変化させる軸です"),
           "launch: 変化させる軸は固定できない");
    expect(fails_with(ex({ "--count", "3", "--out", "o", "--sweep", "fo", "--fixed", "tx=1.5" }), "--fixed の率は 0〜1"),
           "launch: 固定値が範囲外");
    expect(fails_with(ex({ "--count", "3", "--out", "o", "--sweep", "fo", "--fixed", "tx" }), "--fixed は「軸=率」"),
           "launch: 固定値の書式");
    expect(fails_with(ex({ "--count", "3", "--out", "o", "--sweep", "fo", "--fixed", "tx=0,tx=1" }), "--fixed で tx が2回"),
           "launch: 固定値の重複");
    expect(fails_with(ex({ "--count", "3", "--out", "o", "--base", "a.wav" }), "--base は export では使えません"),
           "launch: export は音声をセッションからだけ取る");
    expect(fails_with(ex({ "--count", "3", "--out", "o", "--divisions", "3" }), "--divisions は export では使えません"),
           "launch: 自動生成の設定は export で使えない");

    // convert
    {
        const Result<LaunchOptions> r = parse_launch_options(V { "convert", "anchors.json", "--out", "s.json" });
        expect(r.ok() && r.value.command == LaunchCommand::Convert && r.value.input == "anchors.json" && r.value.out == "s.json"
                 && r.value.base.empty(),
               "launch: convert は入力と --out だけでよい");
    }
    expect(fails_with(parse_launch_options(V { "convert", "anchors.json" }), "convert には --out"), "launch: convert は --out が必須");
    expect(fails_with(parse_launch_options(V { "convert", "anchors.json", "--out", "s", "--base", "a.wav" }),
                      "--base と --target は両方"),
           "launch: convert で音声が片方だけ");
    expect(fails_with(parse_launch_options(V { "convert", "anchors.json", "--out", "s", "--count", "3" }),
                      "--count は convert では使えません"),
           "launch: 書き出しの設定は convert で使えない");

    // auto-anchors
    {
        const Result<LaunchOptions> r = parse_launch_options(
          V { "auto-anchors", "--base", "a.wav", "--target", "b.wav", "--transcript", "はい", "--divisions", "3",
              "--formant-window=80", "--max-formant", "5000", "--acoustic-model", "m", "--dictionary", "d", "--out", "s.json" });
        const AutoAnchorSettings& a = r.value.auto_settings;
        expect(r.ok() && r.value.command == LaunchCommand::AutoAnchors && r.value.out == "s.json" && r.value.transcript == "はい"
                 && a.divisions == 3 && a.formant_window_ms == 80 && near(a.formant.max_formant_hz, 5000.0)
                 && a.align.acoustic_model == "m" && a.align.dictionary == "d",
               "launch: auto-anchors の設定を読める" + (r.ok() ? "" : "（" + r.error + "）"));
    }
    {
        const Result<LaunchOptions> r = parse_launch_options(V { "auto-anchors", "--session", "s.json", "--out", "o.json" });
        const AutoAnchorSettings&   a = r.value.auto_settings;
        expect(r.ok() && a.divisions == 2 && a.formant_window_ms == 50 && near(a.formant.max_formant_hz, 5500.0)
                 && a.align.acoustic_model == "japanese_mfa",
               "launch: auto-anchors の既定は GUI と同じ。セッションがあれば --transcript は要らない");
    }
    const auto au = [&](V extra) {
        V v { "auto-anchors", "--base", "a.wav", "--target", "b.wav" };
        v.insert(v.end(), extra.begin(), extra.end());
        return parse_launch_options(v);
    };
    expect(fails_with(au({ "--out", "o.json" }), "auto-anchors には --transcript"), "launch: 書き起こしの無い auto-anchors");
    expect(fails_with(au({ "--transcript", "x" }), "auto-anchors には --out"), "launch: auto-anchors は --out が必須");
    expect(fails_with(parse_launch_options(V { "auto-anchors", "--transcript", "x", "--out", "o" }),
                      "auto-anchors には --base と --target、または --session"),
           "launch: auto-anchors で入力が無い");
    expect(fails_with(au({ "--transcript", "x", "--out", "o", "s.json" }), "知らない引数です: s.json"),
           "launch: auto-anchors はオプション名の無い引数を取らない");
    expect(fails_with(au({ "--transcript", "x", "--out", "o", "--divisions", "11" }), "--divisions は 1〜10"),
           "launch: 分割数が範囲外");
    expect(fails_with(au({ "--transcript", "x", "--out", "o", "--formant-window", "4" }), "--formant-window は 5〜500"),
           "launch: 窓幅が範囲外");
    expect(fails_with(au({ "--transcript", "x", "--out", "o", "--max-formant", "abc" }), "--max-formant は 2000〜10000"),
           "launch: 最大フォルマントが数でない");
    expect(fails_with(au({ "--transcript", "x", "--out", "o", "--acoustic-model=" }), "--acoustic-model の値が空です"),
           "launch: モデル名が空");

    // 相対パスは絶対パスにし、空の項目はそのまま。
    LaunchOptions o;
    o.base = u8"sub/../音声.wav";
    const std::filesystem::path expect_path =
      (std::filesystem::current_path() / std::filesystem::u8path(u8"音声.wav")).lexically_normal();
    o.out   = "out";
    o.input = "in.json";
    make_paths_absolute(o);
    expect(std::filesystem::u8path(o.base) == expect_path && o.target.empty()
             && std::filesystem::u8path(o.out) == std::filesystem::current_path() / "out"
             && std::filesystem::u8path(o.input) == std::filesystem::current_path() / "in.json",
           "launch: 相対パスを絶対パスにする（日本語を含んでも UTF-8 のまま）");
}

// ── アンカー自動生成の一続きの処理（Python のツールは偽物） ──────────
// 音素を 0.05 s から 0.06 s ずつ並べ、フォルマントは一定の値にした偽物のツール。
AutoAnchorTools fake_tools(std::vector<std::string> base_phones, std::vector<std::string> target_phones,
                           bool ready = true) {
    AutoAnchorTools t;
    t.check = [ready](const AlignParams&) {
        SpeechEnvStatus st;
        st.ready = ready;
        if (!ready) st.problems = { "Python が見つかりません" };
        return st;
    };
    t.formants = [](const std::string&, const FormantParams& p) {
        Formants f;
        for (int k = 0; k < p.num_tracks; ++k) {
            FormantTrack tr;
            for (int i = 0; i <= 60; ++i) {
                tr.t.push_back(i * 0.005);
                tr.hz.push_back(500.0 + 1000.0 * k);
                tr.erb.push_back(freqscale::hz_to_erb(tr.hz.back()));
            }
            f.tracks.push_back(std::move(tr));
        }
        return Result<Formants>::success(std::move(f));
    };
    t.align = [base_phones, target_phones](const std::string& wav, const std::string&, const AlignParams&) {
        const std::vector<std::string>& ph = wav.find("b.wav") != std::string::npos ? target_phones : base_phones;
        Segmentation                    seg;
        SegTier                         tier;
        tier.name = "phones";
        double t0 = 0.0;
        tier.intervals.push_back({ 0.0, 0.05, "sil" });
        t0 = 0.05;
        for (const std::string& l : ph) {
            tier.intervals.push_back({ t0, t0 + 0.06, l });
            t0 += 0.06;
        }
        seg.tiers.push_back(std::move(tier));
        return Result<Segmentation>::success(std::move(seg));
    };
    t.terminate = []() {};
    return t;
}

void test_auto_anchor_pipeline() {
    AutoAnchorSettings s;
    s.transcript = "はい";
    const AutoAnchorSource b { "/x/a.wav", 0.3 }, t { "/x/b.wav", 0.3 };
    const auto             quiet = [](const std::string&) {};

    const AutoAnchorPipelineResult ok = build_auto_anchors(b, t, s, fake_tools({ "h", "a", "i" }, { "h", "a", "i" }), quiet);
    expect(ok.ok() && ok.anchors.size() > 1 && ok.n_freq > 0 && ok.base_phones == "h a i",
           "auto pipeline: 偽物のツールの結果からアンカーを作る" + (ok.ok() ? "" : "（" + ok.error + "）"));
    const AutoAnchorPipelineResult d3 = [&]() {
        AutoAnchorSettings s3 = s;
        s3.divisions          = 3;
        return build_auto_anchors(b, t, s3, fake_tools({ "h", "a", "i" }, { "h", "a", "i" }), quiet);
    }();
    expect(d3.ok() && d3.anchors.size() > ok.anchors.size(), "auto pipeline: 分割数を増やせば時間アンカーが増える");

    const AutoAnchorPipelineResult mm = build_auto_anchors(b, t, s, fake_tools({ "h", "a", "i" }, { "h", "a" }), quiet);
    expect(!mm.ok() && mm.error.find("h a i") != std::string::npos && mm.error.find("h a）") != std::string::npos,
           "auto pipeline: 音素の数が違えば、両方の音素列を添えて失敗");
    const AutoAnchorPipelineResult nr = build_auto_anchors(b, t, s, fake_tools({ "a" }, { "a" }, false), quiet);
    expect(!nr.ok() && nr.error.find("Python が見つかりません") != std::string::npos,
           "auto pipeline: 環境が使えなければ、理由を添えて失敗");
    AutoAnchorSettings empty = s;
    empty.transcript.clear();
    expect(!build_auto_anchors(b, t, empty, fake_tools({ "a" }, { "a" }), quiet).ok(), "auto pipeline: 書き起こしが無ければ失敗");

    // 中止: 音素アライメントは terminate が呼ばれるまで終わらない（MFA が走っている間に Ctrl+C）。
    std::atomic<bool> cancel { true }, stopped { false };
    AutoAnchorTools   slow = fake_tools({ "a" }, { "a" });
    slow.align = [&stopped, inner = slow.align](const std::string& w, const std::string& x, const AlignParams& p) {
        while (!stopped.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return inner(w, x, p);
    };
    slow.terminate = [&stopped]() { stopped.store(true); };
    const AutoAnchorPipelineResult cr = build_auto_anchors(b, t, s, slow, quiet, &cancel);
    expect(cr.canceled && !cr.ok() && stopped.load(), "auto pipeline: 中止が立てば、実行中のツールを止めて canceled で返す");

    const nlohmann::json rec = nlohmann::json::parse(auto_anchor_record_json(s, ok));
    expect(rec["max_formant_hz"] == 5500.0 && rec["acoustic_model"] == "japanese_mfa" && rec["target_phones"] == "h a i"
             && rec["time_anchors"] == ok.anchors.size(),
           "auto pipeline: 記録に設定と音素列と個数を残す");
}

// ── サブコマンド（auto-anchors・convert・export） ─────────────
// 一時ファイルに書かれた内容を読み出す。
std::string read_all(std::FILE* f) {
    std::string s;
    std::rewind(f);
    char buf[512];
    while (std::fgets(buf, sizeof buf, f)) s += buf;
    return s;
}

// 書き出したセッションの JSON を読む（読めなければ discarded）。
nlohmann::json read_json(const std::filesystem::path& p) {
    std::ifstream      is(p);
    std::ostringstream ss;
    ss << is.rdbuf();
    return nlohmann::json::parse(ss.str(), nullptr, false);
}

void test_cli_commands() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path  dir = fs::temp_directory_path() / u8"morphaligner_cli_test";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // 0.3 秒の合成音を2本（F0 違い）書き、それを指すセッションを作る。
    constexpr int fs_hz = 16000;
    for (const auto& [name, f0] : { std::pair { "a.wav", 150.0 }, std::pair { "b.wav", 200.0 } }) {
        std::vector<double> x(fs_hz * 3 / 10);
        for (std::size_t i = 0; i < x.size(); ++i)
            for (int h = 1; h <= 10; ++h)
                x[i] += 0.3 / h * std::sin(2.0 * 3.14159265358979 * f0 * h * static_cast<double>(i) / fs_hz);
        expect(write_wav((dir / name).u8string(), x, fs_hz).ok(), std::string { "cli: テスト用の WAV を書ける: " } + name);
    }
    SessionFile sf;
    sf.base_path   = (dir / "a.wav").u8string();
    sf.target_path = (dir / "b.wav").u8string();
    sf.anchors     = { Anchor { 0.1, 0.12, {} } };
    std::ofstream(dir / "s.json") << session_to_json(sf);

    // 引数を解釈して、サブコマンドを走らせる（log_text に標準エラーの内容）。
    const AutoAnchorTools tools = fake_tools({ "h", "a", "i" }, { "h", "a", "i" });
    const auto run = [&](std::vector<std::string> args, std::string* out_text = nullptr, std::string* log_text = nullptr) {
        const Result<LaunchOptions> o = parse_launch_options(args);
        if (!o.ok()) return -1;
        std::FILE* out = std::tmpfile();
        std::FILE* log = std::tmpfile();
        int        rc  = -1;
        switch (o.value.command) {
        case LaunchCommand::AutoAnchors: rc = run_auto_anchors_command(o.value, out, log, &tools); break;
        case LaunchCommand::Convert: rc = run_convert_command(o.value, out, log); break;
        case LaunchCommand::Export: rc = run_export_command(o.value, out, log); break;
        default: break;
        }
        if (out_text) *out_text = read_all(out);
        if (log_text) *log_text = read_all(log);
        std::fclose(out);
        std::fclose(log);
        return rc;
    };
    const fs::path od   = dir / u8"出力";
    const auto     exp3 = [&](std::vector<std::string> extra, std::string* out_text = nullptr) {
        std::vector<std::string> args { "export", (dir / "s.json").u8string(), "--count", "3", "--out", od.u8string(),
                                        "--prefix", "x" };
        args.insert(args.end(), extra.begin(), extra.end());
        return run(args, out_text);
    };

    // export
    std::string listed;
    expect(exp3({}, &listed) == 0, "cli export: セッションから書き出せる（出力先のフォルダは作る）");
    expect(fs::exists(od / "x_r000.wav", ec) && fs::exists(od / "x_r050.wav", ec) && fs::exists(od / "x_r100.wav", ec)
             && fs::exists(od / "x_batch.json", ec),
           "cli export: GUI と同じ名前の WAV と JSON が揃う");
    expect(listed.find("x_r050.wav") != std::string::npos && listed.find("x_batch.json") != std::string::npos,
           "cli export: 書き出したファイルを標準出力に並べる");
    expect(exp3({}) == 1, "cli export: 同じ名前のファイルがあれば中止する");
    expect(exp3({ "--overwrite" }) == 0, "cli export: --overwrite なら上書きする");
    expect(exp3({ "--sweep", "fo", "--no-manifest" }) == 0 && fs::exists(od / "x_fo_r050.wav", ec)
             && !fs::exists(od / "x_fo_batch.json", ec),
           "cli export: 軸を選べば軸の名前入りのファイル名、--no-manifest なら JSON を書かない");

    // auto-anchors（Python のツールは偽物）
    const fs::path saved = dir / "auto.json";
    expect(run({ "auto-anchors", "--session", (dir / "s.json").u8string(), "--transcript", "はい", "--out", saved.u8string(),
                 "--divisions", "3" })
             == 0,
           "cli auto-anchors: アンカーを作ってセッションに保存する");
    {
        std::ifstream      is(saved);
        std::ostringstream ss;
        ss << is.rdbuf();
        const Result<SessionFile> back = parse_session_json(ss.str());
        const nlohmann::json      j    = read_json(saved);
        expect(back.ok() && back.value.transcript == "はい" && back.value.anchors.size() > 1
                 && back.value.base_path == sf.base_path && !j.is_discarded() && j["auto_anchors"]["divisions"] == 3
                 && j["auto_anchors"]["base_phones"] == "h a i",
               "cli auto-anchors: 保存したセッションは読み直せて、生成の設定と音素列が残る");
    }
    expect(run({ "auto-anchors", "--base", sf.base_path, "--target", sf.target_path, "--transcript", "はい", "--out",
                 saved.u8string() })
             == 1,
           "cli auto-anchors: 保存先がすでにあれば中止する");
    expect(run({ "auto-anchors", "--session", (dir / "s.json").u8string(), "--out", (dir / "b.json").u8string() }) == 1,
           "cli auto-anchors: 書き起こしがどこにも無ければ失敗");
    expect(run({ "auto-anchors", "--session", (dir / "s.json").u8string(), "--base", sf.base_path, "--target",
                 sf.target_path, "--transcript", "はい", "--out", (dir / "b.json").u8string() })
             == 1,
           "cli auto-anchors: 音声のパスを含むセッションと --base / --target は同時に使えない");
    expect(run({ "export", saved.u8string(), "--count", "3", "--out", od.u8string(), "--prefix", "auto" }) == 0
             && fs::exists(od / "auto_r050.wav", ec),
           "cli export: auto-anchors で作ったセッションから書き出す");
    {
        const nlohmann::json j = read_json(od / "auto_batch.json");
        expect(!j.is_discarded() && j.contains("auto_anchors") && j.contains("batch") && j["anchors"].size() > 1,
               "cli export: セッションの記録（auto_anchors）を書き出しの JSON に引き継ぐ");
    }

    // convert（tcmorph のアンカー JSON ＋ 同じフォルダの manifest.json。mat2json.py が書き出す形）
    const fs::path tc = dir / "for-cpp";
    fs::create_directories(tc, ec);
    const fs::path anchors_json = tc / "anchors.json";
    std::ofstream(anchors_json) << R"({"version": 1, "objects": [
        {"name": "reference", "time_anchor": [0.1], "time_freq_anchor": [[700]]},
        {"name": "target", "time_anchor": [0.12], "time_freq_anchor": [[750]]}]})";
    // fileFullPath は、base を絶対パス、target を manifest.json からの相対パスにする。
    const auto write_manifest = [&](double base_fs, const std::string& target_name) {
        nlohmann::json m = { { "version", 1 }, { "source", "/x/edit.mat" }, { "layout", "reftgt" }, { "suggested_alpha", 0.5 } };
        m["objects"] = nlohmann::json::array(
          { { { "name", "reference" }, { "sampling_frequency", base_fs }, { "fileName", "a.wav" },
              { "fileFullPath", dir.u8string() } },
            { { "name", "target" }, { "sampling_frequency", 16000.0 }, { "fileName", target_name }, { "fileFullPath", ".." } } });
        std::ofstream(tc / "manifest.json") << m.dump(2);
    };
    const auto convert = [&](const fs::path& outp, std::vector<std::string> extra = {}, std::string* log_text = nullptr) {
        std::vector<std::string> args { "convert", anchors_json.u8string(), "--out", outp.u8string() };
        args.insert(args.end(), extra.begin(), extra.end());
        return run(args, nullptr, log_text);
    };
    std::string tc_log;
    expect(convert(dir / "tc.json", {}, &tc_log) == 1 && tc_log.find("manifest.json がありません") != std::string::npos,
           "cli convert: manifest.json が無く --base / --target も無ければ失敗");
    write_manifest(16000.0, "b.wav");
    const fs::path tc_saved = dir / "tc.json";
    expect(convert(tc_saved) == 0, "cli convert: anchors.json と manifest.json からセッションを保存する");
    {
        std::ifstream      is(tc_saved);
        std::ostringstream ss;
        ss << is.rdbuf();
        const Result<SessionFile> back = parse_session_json(ss.str());
        const nlohmann::json      j    = read_json(tc_saved);
        expect(back.ok() && !back.value.anchors_only && back.value.anchors.size() == 1
                 && near(back.value.anchors[0].target_t, 0.12) && fs::u8path(back.value.base_path) == dir / "a.wav"
                 && fs::u8path(back.value.target_path) == dir / "b.wav" && !j.is_discarded()
                 && j["tcmorph"]["source"] == "/x/edit.mat" && j["tcmorph"]["suggested_alpha"] == 0.5
                 && !j["tcmorph"].contains("objects"),
               "cli convert: 保存したセッションは manifest の音声とアンカーを持ち、元の情報が tcmorph に残る");
    }
    expect(convert(tc_saved) == 1, "cli convert: 保存先がすでにあれば中止する");
    expect(run({ "export", tc_saved.u8string(), "--count", "3", "--out", od.u8string(), "--prefix", "tc" }) == 0
             && fs::exists(od / "tc_r050.wav", ec),
           "cli export: convert で作ったセッションから書き出す");
    {
        const nlohmann::json j = read_json(od / "tc_batch.json");
        expect(!j.is_discarded() && j.contains("tcmorph") && j.contains("batch"), "cli export: 書き出しの JSON にも tcmorph が残る");
    }
    expect(run({ "export", anchors_json.u8string(), "--count", "3", "--out", od.u8string(), "--prefix", "raw" }, nullptr,
               &tc_log)
               == 1
             && tc_log.find("先に convert") != std::string::npos,
           "cli export: tcmorph のアンカー JSON は、convert するよう案内して失敗");
    expect(convert(dir / "tc2.json", {}, &tc_log) == 0, "cli convert: 別の保存先なら変換できる");
    expect(run({ "convert", (dir / "s.json").u8string(), "--out", (dir / "tc3.json").u8string() }, nullptr, &tc_log) == 1
             && tc_log.find("そのまま") != std::string::npos,
           "cli convert: セッションを渡せば、そのまま export に渡せると案内して失敗");
    write_manifest(48000.0, "b.wav");
    expect(convert(dir / "tc4.json", {}, &tc_log) == 1
             && tc_log.find("標本化周波数が manifest.json と違います") != std::string::npos && !fs::exists(dir / "tc4.json", ec),
           "cli convert: 音声の標本化周波数が manifest と違えば失敗");
    write_manifest(16000.0, "no_such.wav");
    expect(convert(dir / "tc4.json", {}, &tc_log) == 1 && tc_log.find("音声が見つかりません") != std::string::npos,
           "cli convert: manifest の音声が見つからなければ失敗");
    expect(convert(dir / "tc4.json", { "--base", sf.base_path, "--target", sf.target_path }) == 0,
           "cli convert: --base / --target があれば manifest.json は読まない");
    expect(convert(dir / "tc5.json", { "--base", sf.base_path, "--target", (dir / "nope.wav").u8string() }) == 1,
           "cli convert: --base / --target の音声が無ければ失敗");

    fs::remove_all(dir, ec);
}

// ── セッション JSON ─────────────────────────────────────────
// ── OT による周波数アンカー ─────────────────────────────────
// 周波数ごとのパワーを関数で与えて、1フレーム分（nbin 点、0〜fs/2）を作る。
template <class F>
std::vector<double> make_frame(int nbin, int fs, F power_of_hz) {
    std::vector<double> v(static_cast<std::size_t>(nbin));
    for (int k = 0; k < nbin; ++k) v[static_cast<std::size_t>(k)] = power_of_hz(k * (fs / 2.0) / (nbin - 1));
    return v;
}

// 全フレームが同じ包絡（frame）で、frames フレームぶんのチャンネル。
MorphChannel make_flat_channel(const std::vector<double>& frame, int fs, int frames) {
    MorphChannel c;
    c.fs           = fs;
    c.nbin         = static_cast<int>(frame.size());
    c.fft_size     = (c.nbin - 1) * 2;
    c.n_frames     = frames;
    c.frame_period = kFramePeriodMs;
    c.duration     = (frames - 1) * kFramePeriodMs / 1000.0;
    Eigen::MatrixXd sp(c.nbin, frames);
    for (int i = 0; i < frames; ++i)
        for (int k = 0; k < c.nbin; ++k) sp(k, i) = frame[static_cast<std::size_t>(k)];
    c.world.spectrum_parameter.spectrogram = sp;
    return c;
}

void test_ot_anchors() {
    // static にしておく。MSVC は、ラムダの中で使う constexpr のローカル変数にもキャプチャを求める
    // （GCC は通す。CI の Windows のビルドで落ちた。2026-11-17）。
    static constexpr int fs = 16000, nbin = 513;
    OtAnchorParams p;
    p.min_hz = 1000;
    p.max_hz = 5000;
    p.count  = 3;
    // 設定の範囲・本数・圧縮で1フレームの分位点を求める。
    const auto quantiles = [](const std::vector<double>& v, const OtAnchorParams& q) {
        return spectral_quantiles(v.data(), static_cast<int>(v.size()), fs, q.min_hz, q.max_hz, q.count, q.gamma);
    };

    // 一定の包絡なら、範囲を等分した点になる。
    {
        const std::vector<double> flat = make_frame(nbin, fs, [](double) { return 1.0; });
        const std::vector<double> q    = quantiles(flat, p);
        expect(q.size() == 3 && near(q[0], 2000, 1e-6) && near(q[1], 3000, 1e-6) && near(q[2], 4000, 1e-6),
               "ot: 一定の包絡なら範囲を等分する");
    }
    // 圧縮（gamma）を小さくすると、弱い帯域にも分位点が寄る（強い低域に引きずられにくくなる）。
    {
        const std::vector<double> step = make_frame(nbin, fs, [](double f) { return f < 3000 ? 100.0 : 1.0; });
        OtAnchorParams            p1 = p, p05 = p;
        p1.count = p05.count = 1;
        p1.gamma             = 1.0;
        p05.gamma            = 0.5;
        const double m1      = quantiles(step, p1).at(0);
        const double m05     = quantiles(step, p05).at(0);
        expect(m1 < 3000 && m05 < 3000 && m05 > m1 + 50, "ot: 圧縮を強めると弱い帯域の重みが増す");
    }
    // 山が動けば、分位点も同じだけ動く（山どうしが対応する）。
    {
        const auto bump = [](double c) {
            return [c](double f) { return 1e-6 + std::exp(-0.5 * std::pow((f - c) / 150.0, 2)); };
        };
        OtAnchorParams q1 = p;
        q1.count          = 1;
        q1.gamma          = 1.0;
        const std::vector<double> b = make_frame(nbin, fs, bump(2000)), t = make_frame(nbin, fs, bump(2600));
        const double              mb = quantiles(b, q1).at(0);
        const double              mt = quantiles(t, q1).at(0);
        expect(near(mb, 2000, 20) && near(mt, 2600, 20), "ot: 山の位置の移動が分位点の対応になる");
    }
    expect(quantiles(make_frame(nbin, fs, [](double) { return 0.0; }), p).empty(), "ot: 質量が 0 なら空");

    // 時間アンカーごとに作り直す。時間はそのまま、周波数アンカーは置き換え、両側とも昇順。
    const std::vector<double> fb = make_frame(nbin, fs, [](double f) { return 1.0 / (1.0 + f / 500.0); });
    const std::vector<double> ft = make_frame(nbin, fs, [](double f) { return 1.0 / (1.0 + f / 1500.0); });
    const MorphChannel        cb = make_flat_channel(fb, fs, 100), ct = make_flat_channel(ft, fs, 100);
    const std::vector<Anchor> in = { { 0.1, 0.12, { { 700, 800 } } }, { 0.3, 0.35, {} } };
    OtAnchorParams            pa = p;
    pa.count                     = 5;
    const OtAnchorResult r       = make_ot_freq_anchors(cb, ct, in, pa);
    bool ok = r.ok() && r.anchors.size() == 2 && r.n_freq == 10 && r.skipped == 0 && near(r.anchors[0].base_t, 0.1)
           && near(r.anchors[1].target_t, 0.35);
    for (const Anchor& a : r.anchors) {
        ok &= a.freqs.size() == 5;
        for (std::size_t j = 1; j < a.freqs.size(); ++j)
            ok &= a.freqs[j].base_f > a.freqs[j - 1].base_f && a.freqs[j].target_f > a.freqs[j - 1].target_f;
    }
    expect(ok, "ot: 時間アンカーはそのままで、周波数アンカーを昇順に作り直す" + (r.ok() ? "" : "（" + r.error + "）"));
    // target の方が高域寄りの包絡なので、同じ分位点は target の方が高い周波数になる。
    expect(r.ok() && r.anchors[0].freqs[2].target_f > r.anchors[0].freqs[2].base_f,
           "ot: 高域寄りの包絡の側で分位点が高くなる");

    // 無音に近い時刻（最大のフレームより 40 dB 以上小さい）には打たない。
    {
        MorphChannel quiet = cb;
        quiet.world.spectrum_parameter.spectrogram.rightCols(50) *= 1e-6;    // 後半を -60 dB に
        const OtAnchorResult rq = make_ot_freq_anchors(quiet, ct, { { 0.1, 0.1, {} }, { 0.45, 0.45, {} } }, pa);
        expect(rq.ok() && rq.skipped == 1 && rq.anchors[0].freqs.size() == 5 && rq.anchors[1].freqs.empty()
                 && !rq.warnings.empty(),
               "ot: 無音に近い時間アンカーには周波数アンカーを打たない");
    }
    expect(!make_ot_freq_anchors(cb, ct, {}, pa).ok(), "ot: 時間アンカーが無ければ失敗");
    OtAnchorParams bad = pa;
    bad.max_hz         = bad.min_hz;
    expect(!make_ot_freq_anchors(cb, ct, in, bad).ok(), "ot: 周波数範囲が不正なら失敗");

    // ── 帯ごと ──
    // 一定の包絡どうしなら、帯の中の分位点は帯を等分した点になり、その対応がそのまま出る。
    const std::vector<double> flat = make_frame(nbin, fs, [](double) { return 1.0; });
    const MorphChannel        cf   = make_flat_channel(flat, fs, 100);
    OtAnchorParams            pb;
    pb.mode       = OtMode::Bands;
    pb.band_count = 2;
    pb.min_hz     = 300;
    pb.max_hz     = 5000;
    {
        // 既存の周波数アンカー 2 本（base 1000→1200, 2500→2800）で帯が 3 つ: [300,1000]↔[300,1200]、
        // [1000,2500]↔[1200,2800]、[2500,5000]↔[2800,5000]。各帯に 2 本ずつ足して計 8 本。
        const std::vector<Anchor> ain = { { 0.2, 0.2, { { 2500, 2800 }, { 1000, 1200 } } } };
        const OtAnchorResult      rb  = make_ot_freq_anchors(cf, cf, ain, pb);
        const auto&               f   = rb.anchors.at(0).freqs;
        bool ok = rb.ok() && rb.n_freq == 6 && rb.n_edges == 2 && f.size() == 8;
        for (std::size_t j = 1; ok && j < f.size(); ++j)
            ok &= f[j].base_f > f[j - 1].base_f && f[j].target_f > f[j - 1].target_f;
        expect(ok, "ot 帯ごと: 既存の周波数アンカーの間の帯に足し、全体が昇順になる"
                     + (rb.ok() ? "" : "（" + rb.error + "）"));
        // 真ん中の帯（1000–2500 ↔ 1200–2800）を 3 等分した点: 1500→1733.3、2000→2266.7。
        // 既存のアンカーは元の値のまま残る。
        ok = ok && near(f[2].base_f, 1000) && near(f[2].target_f, 1200) && near(f[3].base_f, 1500, 1e-6)
          && near(f[3].target_f, 1200 + 1600.0 / 3, 1e-6) && near(f[4].base_f, 2000, 1e-6)
          && near(f[4].target_f, 1200 + 3200.0 / 3, 1e-6) && near(f[5].base_f, 2500) && near(f[5].target_f, 2800);
        expect(ok, "ot 帯ごと: 帯の中は対応する帯どうしの分位点、既存のアンカーはそのまま");
    }
    {
        // 周波数アンカーが無い時間アンカーは、範囲全体が1つの帯になる。
        const OtAnchorResult rb = make_ot_freq_anchors(cf, cf, { { 0.2, 0.2, {} } }, pb);
        expect(rb.ok() && rb.anchors[0].freqs.size() == 2 && rb.n_edges == 0, "ot 帯ごと: アンカーが無ければ範囲全体が1帯");
    }
    {
        // 交差した周波数アンカー（base の順と target の順が逆）は帯が対応しないので、元のまま。
        const std::vector<Anchor> ain = { { 0.2, 0.2, { { 1000, 2000 }, { 2000, 1500 } } } };
        const OtAnchorResult      rb  = make_ot_freq_anchors(cf, cf, ain, pb);
        expect(rb.ok() && rb.crossed == 1 && rb.n_freq == 0 && rb.anchors[0].freqs.size() == 2
                 && near(rb.anchors[0].freqs[0].base_f, 1000) && !rb.warnings.empty(),
               "ot 帯ごと: 交差したアンカーの時刻にはかけない");
    }
    {
        // 狭すぎる帯（50 Hz 未満）には足さない: 1000–1030 の帯を除く 2 帯に 2 本ずつ。
        const std::vector<Anchor> ain = { { 0.2, 0.2, { { 1000, 1000 }, { 1030, 1030 } } } };
        const OtAnchorResult      rb  = make_ot_freq_anchors(cf, cf, ain, pb);
        expect(rb.ok() && rb.n_freq == 4 && rb.anchors[0].freqs.size() == 6, "ot 帯ごと: 狭すぎる帯には足さない");
    }
}

// ── 包絡の凹凸を戻す ────────────────────────────────────────
// 1 kHz 幅くらいの山を1つ持つ包絡（線形パワー、0〜fs/2 を nbin 点）。
Eigen::VectorXd peaked(int nbin, int fs, double center_hz) {
    Eigen::VectorXd v(nbin);
    for (int k = 0; k < nbin; ++k) {
        const double f = k * (fs / 2.0) / (nbin - 1);
        v[k]           = 1e-3 + std::exp(-0.5 * std::pow((f - center_hz) / 250.0, 2));
    }
    return v;
}

// 合成音（倍音列の振幅を formants の山で重み付けしたもの）を WAV にして解析する。
// 低い倍音にも 1/h の振幅を残す（無いと基本波が弱すぎて Harvest が F0 を見つけられない）。
std::shared_ptr<const MorphChannel> analyze_vowel(const std::vector<double>& formants, const std::string& name) {
    constexpr int       fs = 16000;
    std::vector<double> x(fs * 4 / 10);
    for (int h = 1; h * 150.0 < fs / 2.0; ++h) {
        double amp = 0.3 / h;
        for (double fc : formants) amp += std::exp(-0.5 * std::pow((h * 150.0 - fc) / 120.0, 2));
        for (std::size_t i = 0; i < x.size(); ++i)
            x[i] += 0.05 * amp * std::sin(2.0 * 3.14159265358979 * 150.0 * h * static_cast<double>(i) / fs);
    }
    const std::filesystem::path wav = std::filesystem::temp_directory_path() / name;
    write_wav(wav.u8string(), x, fs);
    Result<AnalyzedAudio> r = analyze_file(wav.u8string());
    std::error_code       ec;
    std::filesystem::remove(wav, ec);
    return r.ok() ? r.value.channel : nullptr;
}

void test_restore_contrast() {
    static constexpr int fs = 16000, nbin = 513;    // static の理由は test_ot_anchors と同じ
    const auto    hz = [](int k) { return k * (fs / 2.0) / (nbin - 1); };

    // 3つとも同じなら何もしない。
    {
        Eigen::MatrixXd       mix = peaked(nbin, fs, 1500);
        const Eigen::MatrixXd ref = mix, tgt = mix, before = mix;
        const double          g   = restore_envelope_contrast(mix, ref, tgt, 0.5, fs);
        expect(near(g, 1.0) && (mix - before).cwiseAbs().maxCoeff() == 0.0, "contrast: 同じ包絡どうしなら何もしない");
    }
    // 山の位置がずれた2つを dB で平均すると平らになる。戻すと凹凸が補間した値に戻る。
    {
        const Eigen::VectorXd ref = peaked(nbin, fs, 1200), tgt = peaked(nbin, fs, 1800);
        Eigen::MatrixXd       mix =
          (0.5 * (ref.array().log10() + tgt.array().log10())).unaryExpr([](double v) { return std::pow(10.0, v); });
        const Eigen::VectorXd before   = mix.col(0);
        const double          want     = 0.5 * (envelope_contrast(ref, fs) + envelope_contrast(tgt, fs));
        const double          g        = restore_envelope_contrast(mix, ref, tgt, 0.5, fs);
        const Eigen::VectorXd after    = mix.col(0);
        expect(envelope_contrast(before, fs) < want * 0.95 && g > 1.0,
               "contrast: 山のずれた平均は平らになり、戻す倍率は 1 より大きい");
        expect(near(envelope_contrast(after, fs), want, want * 1e-6), "contrast: 凹凸は両端の凹凸を補間した値になる");
        // フレームのパワーの和は変わらない。範囲（300〜5500 Hz）の外と範囲の両端は、音量を戻すための
        // 一律の倍率がかかるだけで形は変わらない（範囲の端で段差ができない）。
        const double ratio0 = after[0] / before[0];
        bool         shape  = true;
        for (int k = 0; k < nbin; ++k)
            if (hz(k) < 300.0 || hz(k) > 5500.0) shape &= near(after[k] / before[k], ratio0, 1e-9);
        const int lo = static_cast<int>(std::ceil(300.0 / hz(1))), hi = static_cast<int>(std::floor(5500.0 / hz(1)));
        shape &= near(after[lo] / before[lo], ratio0, 1e-9) && near(after[hi] / before[hi], ratio0, 1e-9);
        expect(near(after.sum(), before.sum(), before.sum() * 1e-9) && shape,
               "contrast: フレームのパワーの和、範囲の外と両端の形は変わらない");
    }
    // 倍率には上限（2 倍）がある。
    {
        const Eigen::VectorXd ref = peaked(nbin, fs, 1500);
        Eigen::MatrixXd       mix = Eigen::VectorXd::Constant(nbin, 1.0) + 1e-3 * ref;    // ほぼ平坦
        const double          g   = restore_envelope_contrast(mix, ref, ref, 0.5, fs);
        expect(near(g, 2.0), "contrast: 倍率は 2 倍まで");
    }

    // モーフィング全体で: 山の位置が違う2つの合成音（周波数アンカーなし）。
    const auto b = analyze_vowel({ 700, 1200, 2600 }, "morphaligner_vowel_b.wav");
    const auto t = analyze_vowel({ 500, 1700, 2300 }, "morphaligner_vowel_t.wav");
    if (!b || !t) {
        expect(false, "contrast: テスト用の合成音を解析できる");
        return;
    }
    MorphPostOptions on;
    on.envelope = EnvelopeMix::AverageRestore;
    const MorphOutput off5 = morphing_channels(*b, *t, {}, MorphRates::uniform(0.5));
    const MorphOutput on5  = morphing_channels(*b, *t, {}, MorphRates::uniform(0.5), on);
    double            c_off = 0.0, c_on = 0.0;
    for (int j = 0; j < off5.morphed.n_frames; ++j) {
        c_off += envelope_contrast(off5.morphed.sp().col(j), b->fs);
        c_on += envelope_contrast(on5.morphed.sp().col(j), b->fs);
    }
    expect(off5.ok() && on5.ok() && on5.contrast_gain > 1.0 && off5.contrast_gain == 0.0 && c_on > c_off * 1.02
             && on5.wave.size() == off5.wave.size(),
           "contrast: 率 0.5 のモーフィングで凹凸が戻る");
    // スペクトルの率が 0 / 1 では何もしない（音はまったく同じ）。
    MorphRates r0 = MorphRates::uniform(0.5);
    r0.sl         = 0.0;
    const MorphOutput a0 = morphing_channels(*b, *t, {}, r0), b0 = morphing_channels(*b, *t, {}, r0, on);
    expect(a0.wave == b0.wave && b0.contrast_gain == 0.0, "contrast: スペクトルの率が 0 なら元の音のまま");
}

// ── 包絡を OT で補間する ────────────────────────────────────
void test_envelope_transport() {
    static constexpr int nbin = 513;    // static の理由は test_ot_anchors と同じ
    // bin の位置 c に幅 8 bin の山を1つ持つ包絡（線形パワー）。
    const auto peak = [](double c) {
        std::vector<double> v(nbin);
        for (int k = 0; k < nbin; ++k) v[static_cast<std::size_t>(k)] = 1e-4 + std::exp(-0.5 * std::pow((k - c) / 8.0, 2));
        return v;
    };
    const auto db = [](double p) { return 10.0 * std::log10(p); };
    const std::vector<double> ref = peak(100), tgt = peak(160);
    std::vector<double>       avg(nbin);
    for (int k = 0; k < nbin; ++k) avg[k] = std::sqrt(ref[k] * tgt[k]);    // dB の平均（tcmorph と同じ）
    constexpr double a = 20.0, b = 300.0;

    // 率 0.5: 平均は2つの山の間でほぼ平らになるが、運ぶと中間（bin 130）に同じ高さ・幅の山が1つできる。
    {
        std::vector<double> out = avg;
        transport_band(ref.data(), tgt.data(), out.data(), nbin, a, b, a, b, 0.5, 1.0);
        const int top = static_cast<int>(std::max_element(out.begin() + 21, out.begin() + 300) - out.begin());
        int       wide = 0;
        for (int k = 21; k < 300; ++k) wide += out[k] > 0.5 * out[top];
        expect(std::abs(top - 130) <= 1 && out[top] > 0.9 && avg[130] < 0.01 && std::abs(wide - 19) <= 2,
               "transport: ずれた山は、中間の位置に同じ高さ・幅の山が1つになる");
        // 帯の外は触らず、帯の端のすぐ内側は平均とつながっている。
        bool outside = true;
        for (int k = 0; k <= 20; ++k) outside &= out[k] == avg[k];
        for (int k = 300; k < nbin; ++k) outside &= out[k] == avg[k];
        expect(outside && std::fabs(db(out[21]) - db(avg[21])) < 0.5 && std::fabs(db(out[299]) - db(avg[299])) < 0.5,
               "transport: 帯の外は平均のまま、帯の端で段差ができない");
    }
    // 率 0 なら ref に戻る（bin の幅で均した分の小さな誤差だけ）。
    {
        std::vector<double> out = ref;
        transport_band(ref.data(), tgt.data(), out.data(), nbin, a, b, a, b, 0.0, 0.3);
        double worst = 0.0;
        for (int k = 21; k < 300; ++k) worst = std::max(worst, std::fabs(db(out[k]) - db(ref[k])));
        expect(worst < 0.5, "transport: 率 0 なら base の包絡に戻る");
    }
    // 帯の端は側ごとに持てる: base の [20, 200] と target の [40, 260] を率 0.5 で補間すると [30, 230] になる。
    // 一定の包絡どうしなら、帯の中も一定（端の値 = 両側の値の幾何補間 = 1）。帯の外は触らない。
    {
        const std::vector<double> one(nbin, 1.0);
        std::vector<double>       out(nbin, 0.5);
        transport_band(one.data(), one.data(), out.data(), nbin, 20, 200, 40, 260, 0.5, 1.0);
        bool flat = true;
        for (int k = 31; k <= 229; ++k) flat &= near(out[k], 1.0, 1e-9);
        expect(flat && out[30] == 0.5 && out[230] == 0.5 && out[25] == 0.5,
               "transport: 側ごとの帯の端から補間後の帯が決まり、その外は触らない");
    }

    // 中間の周波数軸上の周波数アンカー（tcmorph と同じ計算）。
    {
        const std::vector<double> r = { 700, 1200 }, t = { 500, 1700 };
        const auto                m0 = morph_axis_freq_anchors(r, t, 0.0, 16000), m1 = morph_axis_freq_anchors(r, t, 1.0, 16000);
        const auto                mh = morph_axis_freq_anchors(r, t, 0.5, 16000);
        expect(m0.size() == 2 && near(m0[0], 700, 1e-6) && near(m0[1], 1200, 1e-6) && near(m1[0], 500, 1e-6)
                 && near(m1[1], 1700, 1e-6) && mh.size() == 2 && mh[0] > 500 && mh[0] < 700 && mh[1] > 1200 && mh[1] < 1700,
               "transport: 中間の軸の周波数アンカーは fx の率で base → target に動く");
        expect(morph_axis_freq_anchors({ 700, 1200 }, { 1500, 900 }, 0.5, 16000).empty(),
               "transport: target 側が逆転していれば帯を決めない");
    }

    // モーフィング全体で（test_restore_contrast と同じ合成音）。
    const auto vb = analyze_vowel({ 700, 1200, 2600 }, "morphaligner_vowel_b.wav");
    const auto vt = analyze_vowel({ 500, 1700, 2300 }, "morphaligner_vowel_t.wav");
    if (!vb || !vt) {
        expect(false, "transport: テスト用の合成音を解析できる");
        return;
    }
    // 仕組みを見る項目は、フォルマントの帯域（300〜5500 Hz）で運ぶ。
    MorphPostOptions ot;
    ot.envelope        = EnvelopeMix::Transport;
    ot.transport_lo_hz = 300.0;
    ot.transport_hi_hz = 5500.0;
    const MorphOutput avg5 = morphing_channels(*vb, *vt, {}, MorphRates::uniform(0.5));
    const MorphOutput ot5  = morphing_channels(*vb, *vt, {}, MorphRates::uniform(0.5), ot);
    double            c_avg = 0.0, c_ot = 0.0;
    bool              power = ot5.ok() && ot5.morphed.n_frames == avg5.morphed.n_frames;
    for (int j = 0; power && j < avg5.morphed.n_frames; ++j) {
        c_avg += envelope_contrast(avg5.morphed.sp().col(j), vb->fs);
        c_ot += envelope_contrast(ot5.morphed.sp().col(j), vb->fs);
        const double pa = avg5.morphed.sp().col(j).sum(), po = ot5.morphed.sp().col(j).sum();
        power &= near(po, pa, pa * 1e-9);
    }
    expect(avg5.ok() && power && ot5.transport_frames > 0 && near(ot5.transport_bands, 1.0) && c_ot > c_avg,
           "transport: 率 0.5 で凹凸が平均より強く、フレームのパワーは平均と同じ");
    // 周波数アンカーがあれば、帯はアンカー（構造）ごとに1つ（端は隣り合うアンカーの中間）。
    const std::vector<Anchor> anc = { { 0.2, 0.2, { { 700, 500 }, { 1200, 1700 }, { 2600, 2300 } } } };
    const MorphOutput         ota = morphing_channels(*vb, *vt, anc, MorphRates::uniform(0.5), ot);
    expect(ota.ok() && near(ota.transport_bands, 3.0) && ota.transport_skipped == 0,
           "transport: 周波数アンカーごとに帯を区切る");
    // スペクトルの率が 0 なら元の音のまま。
    MorphRates r0 = MorphRates::uniform(0.5);
    r0.sl         = 0.0;
    expect(morphing_channels(*vb, *vt, {}, r0).wave == morphing_channels(*vb, *vt, {}, r0, ot).wave,
           "transport: スペクトルの率が 0 なら元の音のまま");
    // 既定の範囲（5000〜12000 Hz）なら、範囲の外は形を変えない（フレームのパワーを戻す一律の倍率だけ）。
    {
        MorphPostOptions hi;
        hi.envelope             = EnvelopeMix::Transport;
        const MorphOutput oh    = morphing_channels(*vb, *vt, {}, MorphRates::uniform(0.5), hi);
        const int         n     = avg5.morphed.nbin;
        const double      hz    = vb->fs / (2.0 * n);    // tcmorph の中間の軸の目盛り
        bool              shape = oh.ok() && oh.transport_frames > 0;
        for (int j = 0; shape && j < avg5.morphed.n_frames; ++j) {
            const auto   a = avg5.morphed.sp().col(j), o = oh.morphed.sp().col(j);
            const double r = o[0] / a[0];
            for (int k = 0; k < n; ++k)
                if (k * hz < 5000.0 - hz) shape &= near(o[k] / a[k], r, 1e-9);
        }
        expect(shape, "transport: 既定の範囲では 5 kHz より下の形は平均のまま");
    }
}

// ── 連続書き出し ────────────────────────────────────────────
void test_batch_export() {
    // 率は 0 と 1 をちょうど含み、等間隔に並ぶ。
    const std::vector<double> r11 = batch_rates(11);
    expect(r11.size() == 11 && near(r11.front(), 0.0) && near(r11.back(), 1.0) && near(r11[5], 0.5),
           "batch: 本数ぶんの率が 0 → 1 に等分される");
    const std::vector<double> r2 = batch_rates(2);
    expect(r2.size() == 2 && near(r2[0], 0.0) && near(r2[1], 1.0), "batch: 2 本なら両端だけ");
    expect(batch_rates(1).empty(), "batch: 2 本未満は空");

    BatchExportSettings ab;
    ab.prefix = "ab";
    expect(batch_wav_name(ab, 0.0) == "ab_r000.wav" && batch_wav_name(ab, 0.5) == "ab_r050.wav"
             && batch_wav_name(ab, 1.0) == "ab_r100.wav" && batch_manifest_name(ab) == "ab_batch.json",
           "batch: 全軸ならファイル名は接頭辞＋百分率のゼロ詰め3桁");
    expect(batch_wav_name(ab, batch_rates(7)[1]) == "ab_r017.wav", "batch: 割り切れない率は四捨五入");

    // 選んだ軸だけを掃引するときは、ファイル名に軸の名前を入れる（別の軸の掃引と上書きし合わない）。
    BatchExportSettings sub = ab;
    for (bool& b : sub.sweep) b = false;
    sub.sweep[2] = sub.sweep[3] = true;    // fo, sl
    expect(batch_wav_name(sub, 0.5) == "ab_fo+sl_r050.wav" && batch_manifest_name(sub) == "ab_fo+sl_batch.json"
             && !batch_sweeps_all_axes(sub) && batch_sweeps_all_axes(ab),
           "batch: 選んだ軸の名前がファイル名に入る");
    MorphPostOptions contrast_on, ot_on;
    contrast_on.envelope   = EnvelopeMix::AverageRestore;
    ot_on.envelope         = EnvelopeMix::Transport;
    ot_on.transport_gamma  = 0.3;
    expect(batch_wav_name(ab, 0.5, contrast_on) == "ab_contrast_r050.wav"
             && batch_wav_name(sub, 0.5, contrast_on) == "ab_fo+sl_contrast_r050.wav"
             && batch_manifest_name(ab, contrast_on) == "ab_contrast_batch.json"
             && batch_wav_name(ab, 0.5, ot_on) == "ab_ot030_5000-12000_r050.wav",
           "batch: 包絡の混ぜ方が平均でなければファイル名に印が入る");

    // 掃引する軸は同じ値で動き、ほかは固定値のまま。
    sub.fixed               = MorphRates { 0.1, 0.2, 0.9, 0.9, 0.3 };
    const MorphRates pr     = batch_point_rates(sub, 0.6);
    const MorphRates pr_all = batch_point_rates(ab, 0.6);
    expect(near(pr.fo, 0.6) && near(pr.sl, 0.6) && near(pr.tx, 0.1) && near(pr.fx, 0.2) && near(pr.ap, 0.3),
           "batch: 掃引する軸は連動し、ほかは固定値");
    expect(near(pr_all.tx, 0.6) && near(pr_all.fx, 0.6) && near(pr_all.fo, 0.6) && near(pr_all.sl, 0.6)
             && near(pr_all.ap, 0.6),
           "batch: 全軸なら全軸一括と同じ");

    // 上限の本数でも名前がぶつからない（3桁に丸めるので、刻みが 1% を割ると重複する）。
    BatchExportSettings many;
    many.prefix = "x";
    std::vector<std::string> names;
    for (double rate : batch_rates(BatchExportSettings::kCountMax)) names.push_back(batch_wav_name(many, rate));
    std::sort(names.begin(), names.end());
    expect(static_cast<int>(names.size()) == BatchExportSettings::kCountMax
             && std::adjacent_find(names.begin(), names.end()) == names.end(),
           "batch: 上限の本数でもファイル名が重複しない");

    // 設定の検査。
    const std::string tmp = std::filesystem::temp_directory_path().u8string();
    BatchExportSettings s;
    s.dir    = tmp;
    s.prefix = "morphaligner_batch_test";
    expect(validate_batch_settings(s).ok(), "batch: 正しい設定は通る");
    {
        BatchExportSettings bad = s;
        bad.count               = BatchExportSettings::kCountMax + 1;
        expect(!validate_batch_settings(bad).ok(), "batch: 本数が範囲外なら失敗");
        bad        = s;
        bad.prefix = "";
        expect(!validate_batch_settings(bad).ok(), "batch: 接頭辞が空なら失敗");
        bad        = s;
        bad.prefix = "a/b";
        expect(!validate_batch_settings(bad).ok(), "batch: 接頭辞にパス区切りが入れば失敗");
        bad     = s;
        bad.dir = (std::filesystem::temp_directory_path() / "morphaligner_no_such_dir").u8string();
        expect(!validate_batch_settings(bad).ok(), "batch: 出力先が無ければ失敗");
        bad = s;
        for (bool& b : bad.sweep) b = false;
        expect(!validate_batch_settings(bad).ok(), "batch: 掃引する軸が無ければ失敗");
        bad.sweep[0] = true;
        bad.fixed.fo = 1.5;    // 固定する軸（fo）の値が範囲外
        expect(!validate_batch_settings(bad).ok(), "batch: 固定する率が範囲外なら失敗");
        bad.sweep[2] = true;    // fo も掃引するなら fixed.fo は使わないので問わない
        expect(validate_batch_settings(bad).ok(), "batch: 掃引する軸の固定値は問わない");
    }

    // 実際に書き出す。0.3 秒の合成音を自分自身とモーフィングする（アンカーなし）。
    constexpr int       fs = 16000;
    std::vector<double> x(fs * 3 / 10);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        for (int h = 1; h <= 10; ++h) x[i] += 0.3 / h * std::sin(2.0 * 3.14159265358979 * 150.0 * h * t);
    }
    std::error_code             ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "morphaligner_batch_test_dir";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path wav = dir / "src.wav";
    expect(write_wav(wav.u8string(), x, fs).ok(), "batch: テスト用の WAV を書ける");

    const Result<AnalyzedAudio> a = analyze_file(wav.u8string());
    if (!a.ok() || !a.value.channel) {
        expect(false, "batch: テスト用の WAV を解析できる（" + a.error + "）");
        std::filesystem::remove_all(dir, ec);
        return;
    }

    BatchExportRequest req;
    req.base           = a.value.channel;
    req.target         = a.value.channel;
    req.anchors        = { Anchor { 0.1, 0.12, { { 700, 750 } } } };
    req.base_path      = wav.u8string();
    req.target_path    = wav.u8string();
    req.settings       = s;
    req.settings.dir   = dir.u8string();
    req.settings.count = 3;

    BatchExportProgress prog;
    const BatchExportResult out = run_batch_export(req, prog);
    expect(out.ok() && !out.canceled && out.written == 3 && prog.done.load() == 3,
           "batch: 指定した本数だけ書き出せる" + (out.ok() ? "" : "（" + out.error + "）"));
    bool all = true;
    for (double rate : batch_rates(3)) all &= std::filesystem::exists(dir / batch_wav_name(req.settings, rate), ec);
    expect(all, "batch: 率ごとの WAV が揃う（r000 / r050 / r100）");

    // 残した JSON はセッション形式なので、そのまま読み込める。
    expect(!out.manifest.empty() && std::filesystem::exists(std::filesystem::u8path(out.manifest), ec),
           "batch: 設定の JSON を残す");
    {
        std::ifstream     is(std::filesystem::u8path(out.manifest), std::ios::binary);
        std::ostringstream ss;
        ss << is.rdbuf();
        const Result<SessionFile> sf = parse_session_json(ss.str());
        expect(sf.ok() && sf.value.anchors.size() == 1 && near(sf.value.anchors[0].target_t, 0.12)
                 && sf.value.base_path == req.base_path,
               "batch: 残した JSON をセッションとして読める");
    }

    // 書き出したものを数え直すと、WAV 3 本＋JSON で 4 つが上書き対象になる。
    expect(count_existing_batch_files(req.settings) == 4, "batch: 上書きになる既存ファイルを数えられる");

    // 選んだ軸だけの掃引。JSON に掃引した軸・固定値・各ファイルの全軸の率が残る。
    {
        BatchExportRequest sreq = req;
        for (bool& b : sreq.settings.sweep) b = false;
        sreq.settings.sweep[2] = true;    // fo だけ
        sreq.settings.fixed    = MorphRates::uniform(0.0);
        BatchExportProgress     sp;
        const BatchExportResult so = run_batch_export(sreq, sp);
        bool                    ok = so.ok() && so.written == 3
               && std::filesystem::exists(dir / batch_wav_name(sreq.settings, 1.0), ec);
        if (ok) {
            std::ifstream      is(std::filesystem::u8path(so.manifest), std::ios::binary);
            std::ostringstream ss;
            ss << is.rdbuf();
            try {
                const nlohmann::json  j     = nlohmann::json::parse(ss.str());
                const nlohmann::json& batch = j.at("batch");
                const nlohmann::json& files = batch.at("files");
                // 最後のファイル（率 1）は fo だけ 1 で、ほかは固定値の 0。
                const nlohmann::json& last = files.back().at("rates");
                ok = batch.at("sweep") == nlohmann::json::array({ "fo" }) && batch.at("fixed").size() == 4
                  && !batch.at("fixed").contains("fo") && files.size() == 3
                  && files.back().at("file") == batch_wav_name(sreq.settings, 1.0)
                  && near(last.at("fo").get<double>(), 1.0) && near(last.at("tx").get<double>(), 0.0)
                  && near(last.at("ap").get<double>(), 0.0);
            } catch (const std::exception&) {
                ok = false;
            }
        }
        expect(ok, "batch: 選んだ軸だけを掃引して書き出せる" + (so.ok() ? "" : "（" + so.error + "）"));
    }

    // 中止フラグが立っていれば1本も書かずに戻る。
    BatchExportProgress canceled;
    canceled.cancel.store(true);
    const BatchExportResult c = run_batch_export(req, canceled);
    expect(c.ok() && c.canceled && c.written == 0, "batch: 中止されたら書き出さずに戻る");

    std::filesystem::remove_all(dir, ec);
}

void test_session_json() {
    SessionFile s;
    s.base_path   = "C:/音声/base.wav";
    s.target_path = "/tmp/target.wav";
    s.transcript  = "あかさたな";
    s.anchors     = { { 0.1, 0.2, { { 500, 600 }, { 1500, 1400 } } }, { 0.3, 0.35, {} } };

    const Result<SessionFile> rr = parse_session_json(session_to_json(s));
    const SessionFile&        r  = rr.value;
    bool ok = rr.ok() && !r.anchors_only && r.base_path == s.base_path && r.target_path == s.target_path
           && r.transcript == s.transcript && r.anchors.size() == 2 && r.anchors[0].freqs.size() == 2
           && near(r.anchors[0].freqs[1].target_f, 1400) && near(r.anchors[1].target_t, 0.35);
    expect(ok, "session: 保存した JSON を読むと元に戻る");

    const Result<SessionFile> old =
      parse_session_json(R"({"version":1,"waves":{"base":"a.wav","target":"b.wav"},"anchors":[]})");
    expect(old.ok() && old.value.transcript.empty() && old.value.anchors.empty(),
           "session: transcript の無い古い形式も読める");

    expect(fails_with(parse_session_json("{"), "JSON 解析エラー: "), "session: 壊れた JSON は失敗");
    expect(fails_with(parse_session_json(R"({"anchors":[]})"), "waves が不正: "), "session: waves が無ければ失敗");
    expect(fails_with(parse_session_json(
                        R"({"waves":{"base":"a","target":"b"},"anchors":[{"time":{"base":0.1},"freqs":[]}]})"),
                      "anchors が不正: "),
           "session: アンカーの項目が欠けていれば失敗");
    expect(fails_with(parse_session_json(R"({"version":2,"waves":{"base":"a","target":"b"},"anchors":[]})"),
                      "新しい版のアプリで保存された"),
           "session: 新しい version のセッションは断る");
    expect(fails_with(parse_session_json(R"({"version":"1","waves":{"base":"a","target":"b"},"anchors":[]})"),
                      "version が不正"),
           "session: version が整数でなければ失敗");
    expect(parse_session_json(R"({"waves":{"base":"a","target":"b"},"anchors":[]})").ok(),
           "session: version が無ければ 1 とみなして読む");

    // tcmorph のアンカー形式（objects 配列。0 は「アンカーなし」として落とす）。
    const Result<SessionFile> rt = parse_session_json(R"({
        "version": 1,
        "objects": [
          { "name": "A", "time_anchor": [0.2, 0.5], "time_freq_anchor": [[700, 1200], [720]] },
          { "name": "B", "time_anchor": [0.3, 0.6], "time_freq_anchor": [[750, 1300], [760]] }
        ]})");
    const SessionFile& t = rt.value;
    ok = rt.ok() && t.anchors_only && t.base_path.empty() && t.anchors.size() == 2 && near(t.anchors[0].base_t, 0.2)
      && near(t.anchors[0].target_t, 0.3) && t.anchors[0].freqs.size() == 2 && t.anchors[1].freqs.size() == 1
      && near(t.anchors[1].freqs[0].target_f, 760) && !t.notes.empty();
    expect(ok, "session: tcmorph 形式のアンカーを読める");
    expect(fails_with(parse_session_json(R"({"objects":[{"name":"A","time_anchor":[0.2]}]})"),
                      "tcmorph 形式のアンカーが不正: "),
           "session: tcmorph 形式で素材が2つでなければ失敗");
}

}    // namespace

int main() {
    test_freqscale();
    test_anchor_side();
    test_smooth_formants();
    test_build_anchor_matrices();
    test_analysis();
    test_launch_options();
    test_auto_anchors();
    test_ot_anchors();
    test_restore_contrast();
    test_envelope_transport();
    test_batch_export();
    test_auto_anchor_pipeline();
    test_cli_commands();
    test_session_json();

    if (g_failures > 0) {
        std::printf("失敗: %d 件\n", g_failures);
        return 1;
    }
    std::printf("すべて成功\n");
    return 0;
}
