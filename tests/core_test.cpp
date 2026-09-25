// コアのライブラリ（GUI に依存しない部分）の単体テスト。
//
// 対象: 周波数尺度の変換、アンカーの Side による参照、音声の解析（合成音の WAV を一時ディレクトリに作る）、フォルマントの移動平均、モーフィング用のアンカー整形、
// アンカー自動生成、セッション JSON の読み書き。Python は使わないので、
// どこで実行してもよい（CI の build.yml がビルド直後に各 OS で実行する）。
//
// ビルド: cmake -DMORPHALIGNER_BUILD_TESTS=ON ... → bin/core_test

#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include "analysis.hpp"
#include "anchor.hpp"
#include "auto_anchors.hpp"
#include "formant_smoothing.hpp"
#include "freqscale.hpp"
#include "morphing.hpp"
#include "session_io.hpp"

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

// ── セッション JSON ─────────────────────────────────────────
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
    test_auto_anchors();
    test_session_json();

    if (g_failures > 0) {
        std::printf("失敗: %d 件\n", g_failures);
        return 1;
    }
    std::printf("すべて成功\n");
    return 0;
}
