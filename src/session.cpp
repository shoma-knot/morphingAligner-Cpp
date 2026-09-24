#include "session.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <tcmorph/anchor_io.hpp>

#include "app.hpp"
#include "log.hpp"

using json = nlohmann::json;

namespace {

constexpr int kSchemaVersion = 1;

bool file_readable(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

// tcmorph のアンカー JSON（anchor_io.hpp の objects 配列）を App のアンカー列に変換する。
// 並び順で base/target を決める（objects[0]=base, objects[1]=target）。tcmorph 側の
// morph_aligner も同じく「先頭が参照、2番目が目標」として扱う。
// 形式の検証（時間アンカーが正で狭義単調増加・素材間でアンカー本数が一致 など）は
// ParseAnchorSet に任せ、違反は例外で上がってくる。
std::vector<Anchor> anchors_from_tcmorph(const json& root) {
    const tcmorph::io::AnchorSet set = tcmorph::io::ParseAnchorSet(root);
    if (set.objects.size() != 2)
        throw std::invalid_argument("本アプリは base/target の2素材のみ対応です（JSON の素材数: "
                                    + std::to_string(set.objects.size()) + "）");

    for (const std::string& w : set.warnings) applog::add("警告: " + w);
    // tcmorph は逆転した周波数アンカーを「折り返すワープ」として通すが、本アプリは
    // 周波数アンカーに順序の概念がない（UI ではクリック順に並ぶだけ）ため、
    // モーフィング時に base 側の周波数で並べ替える。その旨を補足しておく。
    if (!set.warnings.empty())
        applog::add("補足: 本アプリは周波数アンカーを base 側の周波数順に並べ替えて使うため、"
                    "逆転したアンカーは tcmorph 単体とは異なる結果になります");

    const tcmorph::io::ObjectAnchors& b = set.objects[0];
    const tcmorph::io::ObjectAnchors& t = set.objects[1];
    applog::add("tcmorph 形式のアンカー: base=\"" + b.name + "\" / target=\"" + t.name + "\"");

    // 周波数アンカーは (本数, 時間アンカー) の向きで、末尾が 0 詰め。0 は「そこには
    // アンカーがない」の意味なので落とす（本数が素材間で揃うことは検証済み）。
    const Eigen::Index n_row = std::min(b.time_freq_anchor.rows(), t.time_freq_anchor.rows());

    std::vector<Anchor> out;
    out.reserve(static_cast<std::size_t>(b.time_anchor.size()));
    for (Eigen::Index jj = 0; jj < b.time_anchor.size(); ++jj) {
        Anchor a;
        a.base_t   = b.time_anchor[jj];
        a.target_t = t.time_anchor[jj];
        for (Eigen::Index kk = 0; kk < n_row; ++kk) {
            const double bf = b.time_freq_anchor(kk, jj);
            const double tf = t.time_freq_anchor(kk, jj);
            if (bf != 0.0 && tf != 0.0) a.freqs.push_back(FreqAnchor { bf, tf });
        }
        out.push_back(std::move(a));
    }
    return out;
}

}    // namespace

bool save_session(const App& app, const std::string& path) {
    json j;
    j["version"] = kSchemaVersion;
    j["waves"]   = { { "base", app.base.path }, { "target", app.target.path } };
    // MFA 用の書き起こし（base/target 共通。古いセッションには無いので読み込み側は省略可）。
    j["transcript"] = app.transcript;

    json anchors = json::array();
    for (const Anchor& a : app.anchors) {
        json freqs = json::array();
        for (const FreqAnchor& f : a.freqs)
            freqs.push_back({ { "base", f.base_f }, { "target", f.target_f } });
        anchors.push_back({
          { "time",  { { "base", a.base_t }, { "target", a.target_t } } },
          { "freqs", freqs },
        });
    }
    j["anchors"] = anchors;

    std::ofstream os(path);
    if (!os) {
        applog::add("セッション保存失敗: ファイルを開けません: " + path);
        return false;
    }
    os << j.dump(2) << '\n';
    applog::add("セッション保存しました: " + path);
    return true;
}

SessionLoadData load_session_data(const std::string& path) {
    SessionLoadData d;

    std::ifstream is(path);
    if (!is) {
        applog::add("セッション読み込み失敗: ファイルを開けません: " + path);
        return d;
    }

    json j;
    try {
        is >> j;
    } catch (const std::exception& e) {
        applog::add(std::string { "セッション読み込み失敗: JSON 解析エラー: " } + e.what());
        return d;
    }

    // tcmorph のアンカー JSON は objects 配列を持つ。音声パスを含まないので、
    // アンカーだけを読んで現在の base/target に乗せる（適用は apply_session_data）。
    if (j.contains("objects")) {
        try {
            d.anchors = anchors_from_tcmorph(j);
        } catch (const std::exception& e) {
            applog::add(std::string { "アンカー読み込み失敗: " } + e.what());
            return d;
        }
        applog::add("アンカーを読み込みました: " + path);
        d.anchors_only = true;
        d.ok           = true;
        return d;
    }

    try {
        d.base_path   = j.at("waves").at("base").get<std::string>();
        d.target_path = j.at("waves").at("target").get<std::string>();
    } catch (const std::exception& e) {
        applog::add(std::string { "セッション読み込み失敗: waves が不正: " } + e.what());
        return d;
    }

    // 書き起こしは任意項目（無ければ空のまま）。
    if (const auto it = j.find("transcript"); it != j.end() && it->is_string()) d.transcript = it->get<std::string>();

    // 音声ファイルの存在を先に確認（片方だけ復元して中途半端な状態になるのを防ぐ）。
    if (!file_readable(d.base_path)) {
        applog::add("セッション読み込み失敗: base 音声が見つかりません: " + d.base_path);
        return d;
    }
    if (!file_readable(d.target_path)) {
        applog::add("セッション読み込み失敗: target 音声が見つかりません: " + d.target_path);
        return d;
    }

    // アンカーを先にパースしておく（音声を差し替える前に検証を済ませる）。
    try {
        for (const json& ja : j.at("anchors")) {
            Anchor a;
            a.base_t   = ja.at("time").at("base").get<double>();
            a.target_t = ja.at("time").at("target").get<double>();
            for (const json& jf : ja.at("freqs"))
                a.freqs.push_back(FreqAnchor { jf.at("base").get<double>(), jf.at("target").get<double>() });
            d.anchors.push_back(std::move(a));
        }
    } catch (const std::exception& e) {
        applog::add(std::string { "セッション読み込み失敗: anchors が不正: " } + e.what());
        return d;
    }

    // 音声を解析（デコード失敗などはここで検出）。GL は使わないのでワーカーで実行できる。
    try {
        d.base_spec = analyze_file(d.base_path);
    } catch (const std::exception& e) {
        applog::add(std::string { "セッション読み込み失敗: base 解析エラー: " } + e.what());
        return d;
    }
    try {
        d.target_spec = analyze_file(d.target_path);
    } catch (const std::exception& e) {
        applog::add(std::string { "セッション読み込み失敗: target 解析エラー: " } + e.what());
        return d;
    }

    applog::add("セッション読み込みました: " + path);
    d.ok = true;
    return d;
}

void apply_session_data(App& app, SessionLoadData&& d) {
    if (!d.ok) return;

    // tcmorph 形式は音声を持たないので、現在読み込んである base/target に乗せる。
    if (d.anchors_only) {
        if (!(app.base.loaded() && app.target.loaded())) {
            applog::add("アンカー適用失敗: 先に base / target の音声を読み込んでください");
            return;
        }
        // 音声より後ろのアンカーはモーフィング時に読み飛ばされるので、ここで知らせる。
        int out_of_range = 0;
        for (const Anchor& a : d.anchors)
            if (a.base_t >= app.base.spec.duration || a.target_t >= app.target.spec.duration)
                ++out_of_range;
        if (out_of_range > 0)
            applog::add("警告: 現在の音声の長さを超えるアンカーが " + std::to_string(out_of_range)
                        + " 個あります（モーフィング時に読み飛ばされます）");

        app.anchors = std::move(d.anchors);
        applog::add("アンカーを適用しました: " + std::to_string(app.anchors.size()) + " 個");
        return;
    }

    apply_track(app.base, d.base_path, std::move(d.base_spec));
    apply_track(app.target, d.target_path, std::move(d.target_spec));
    app.anchors    = std::move(d.anchors);
    app.transcript = std::move(d.transcript);
}
