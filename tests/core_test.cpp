// コアのライブラリ（GUI に依存しない部分）の単体テスト。
//
// 対象: 周波数尺度の変換、アンカーの Side による参照、音声の解析（合成音の WAV を一時ディレクトリに作る）、コマンドライン引数、フォルマントの移動平均、モーフィング用のアンカー整形、
// アンカー自動生成、連続書き出し、セッション JSON の読み書き。Python は使わないので、
// どこで実行してもよい（CI の build.yml がビルド直後に各 OS で実行する）。
//
// ビルド: cmake -DMORPHALIGNER_BUILD_TESTS=ON ... → bin/core_test

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "analysis.hpp"
#include "anchor.hpp"
#include "auto_anchors.hpp"
#include "batch_export.hpp"
#include "formant_smoothing.hpp"
#include "freqscale.hpp"
#include "launch_options.hpp"
#include "morphing.hpp"
#include "session_io.hpp"

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
        expect(r.ok() && !r.value.has_files() && !r.value.align, "launch: 引数なしは何もしない");
    }
    {
        const Result<LaunchOptions> r =
          parse_launch_options(V { "--base", "a.wav", "--target=b.wav", "--transcript", "あいうえお", "--align" });
        const LaunchOptions& o = r.value;
        expect(r.ok() && o.base == "a.wav" && o.target == "b.wav" && o.has_transcript && o.transcript == "あいうえお"
                 && o.align && o.session.empty(),
               "launch: 「--name 値」と「--name=値」の両方を読める");
    }
    {
        const Result<LaunchOptions> r = parse_launch_options(V { "--session", "s.json", "--align" });
        expect(r.ok() && r.value.session == "s.json" && r.value.align && !r.value.has_transcript,
               "launch: --session があれば --align に --transcript は要らない（書き起こしはセッションから）");
    }
    {
        const Result<LaunchOptions> r = parse_launch_options(V { "--transcript=" });
        expect(r.ok() && r.value.has_transcript && r.value.transcript.empty(), "launch: 空の書き起こしも指定できる");
    }
    expect(parse_launch_options(V { "--help", "--nope" }).value.help, "launch: --help はほかの誤りより優先");
    expect(parse_launch_options(V { "--version" }).value.version, "launch: --version");
    expect(fails_with(parse_launch_options(V { "a.wav" }), "知らない引数です: a.wav"),
           "launch: オプション名の無い引数は受け付けない");
    expect(fails_with(parse_launch_options(V { "--bass", "a.wav" }), "知らない引数です: --bass"), "launch: 知らないオプション");
    expect(fails_with(parse_launch_options(V { "--base" }), "--base に値がありません"), "launch: 値の無いオプション");
    expect(fails_with(parse_launch_options(V { "--base=" }), "--base の値（パス）が空です"), "launch: 空のパス");
    expect(fails_with(parse_launch_options(V { "--base", "a", "--base", "b" }), "--base が2回指定されています"),
           "launch: 同じオプションの重複");
    expect(fails_with(parse_launch_options(V { "--transcript", "x", "--align" }), "--align には --base"),
           "launch: 音声なしの --align");
    expect(fails_with(parse_launch_options(V { "--base", "a.wav", "--align" }), "--align には --transcript"),
           "launch: 書き起こしなしの --align");

    // 相対パスは絶対パスにし、空の項目はそのまま。
    LaunchOptions o;
    o.base = u8"sub/../音声.wav";
    make_paths_absolute(o);
    const std::filesystem::path expect_path =
      (std::filesystem::current_path() / std::filesystem::u8path(u8"音声.wav")).lexically_normal();
    expect(std::filesystem::u8path(o.base) == expect_path && o.target.empty(),
           "launch: 相対パスを絶対パスにする（日本語を含んでも UTF-8 のまま）");
}

// ── セッション JSON ─────────────────────────────────────────
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
    test_batch_export();
    test_session_json();

    if (g_failures > 0) {
        std::printf("失敗: %d 件\n", g_failures);
        return 1;
    }
    std::printf("すべて成功\n");
    return 0;
}
