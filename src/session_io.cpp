#include "session_io.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <tcmorph/anchor_io.hpp>

using json = nlohmann::json;

namespace {

constexpr int kSchemaVersion = 1;

// 例外の理由に前置きを付けて投げ直す（どの項目が不正かを分かるようにする）。
[[noreturn]] void rethrow_with(const std::string& prefix, const std::exception& e) {
    throw std::runtime_error(prefix + e.what());
}

// tcmorph のアンカー JSON（anchor_io.hpp の objects 配列）を読む。
// 並び順で base/target を決める（objects[0]=base, objects[1]=target）。tcmorph 側の
// morph_aligner も同じく「先頭が参照、2番目が目標」として扱う。
// 形式の検証（時間アンカーが正で狭義単調増加・素材間でアンカー本数が一致 など）は
// ParseAnchorSet に任せ、違反は例外で上がってくる。
SessionFile parse_tcmorph(const json& root) {
    const tcmorph::io::AnchorSet set = tcmorph::io::ParseAnchorSet(root);
    if (set.objects.size() != 2)
        throw std::invalid_argument("本アプリは base/target の2素材のみ対応です（JSON の素材数: "
                                    + std::to_string(set.objects.size()) + "）");

    SessionFile s;
    s.anchors_only = true;
    for (const std::string& w : set.warnings) s.notes.push_back("警告: " + w);
    // tcmorph は逆転した周波数アンカーを「折り返すワープ」として通すが、本アプリは
    // 周波数アンカーに順序の概念がない（UI ではクリック順に並ぶだけ）ため、
    // モーフィング時に base 側の周波数で並べ替える。その旨を補足しておく。
    if (!set.warnings.empty())
        s.notes.push_back("補足: 本アプリは周波数アンカーを base 側の周波数順に並べ替えて使うため、"
                          "逆転したアンカーは tcmorph 単体とは異なる結果になります");

    const tcmorph::io::ObjectAnchors& b = set.objects[0];
    const tcmorph::io::ObjectAnchors& t = set.objects[1];
    s.notes.push_back("tcmorph 形式のアンカー: base=\"" + b.name + "\" / target=\"" + t.name + "\"");

    // 周波数アンカーは (本数, 時間アンカー) の向きで、末尾が 0 詰め。0 は「そこには
    // アンカーがない」の意味なので落とす（本数が素材間で揃うことは検証済み）。
    const Eigen::Index n_row = std::min(b.time_freq_anchor.rows(), t.time_freq_anchor.rows());

    s.anchors.reserve(static_cast<std::size_t>(b.time_anchor.size()));
    for (Eigen::Index jj = 0; jj < b.time_anchor.size(); ++jj) {
        Anchor a;
        a.base_t   = b.time_anchor[jj];
        a.target_t = t.time_anchor[jj];
        for (Eigen::Index kk = 0; kk < n_row; ++kk) {
            const double bf = b.time_freq_anchor(kk, jj);
            const double tf = t.time_freq_anchor(kk, jj);
            if (bf != 0.0 && tf != 0.0) a.freqs.push_back(FreqAnchor { bf, tf });
        }
        s.anchors.push_back(std::move(a));
    }
    return s;
}

}    // namespace

std::string session_to_json(const SessionFile& s) {
    json j;
    j["version"] = kSchemaVersion;
    j["waves"]   = { { "base", s.base_path }, { "target", s.target_path } };
    // MFA 用の書き起こし（base/target 共通。古いセッションには無いので読み込み側は省略可）。
    j["transcript"] = s.transcript;

    json anchors = json::array();
    for (const Anchor& a : s.anchors) {
        json freqs = json::array();
        for (const FreqAnchor& f : a.freqs) freqs.push_back({ { "base", f.base_f }, { "target", f.target_f } });
        anchors.push_back({
          { "time", { { "base", a.base_t }, { "target", a.target_t } } },
          { "freqs", freqs },
        });
    }
    j["anchors"] = anchors;
    return j.dump(2) + '\n';
}

namespace {

// parse_session_json の本体。不正な形式は例外で知らせる（項目ごとの前置きを付けて投げ直す）。
SessionFile parse_session_json_or_throw(const std::string& text) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        rethrow_with("JSON 解析エラー: ", e);
    }

    // tcmorph のアンカー JSON は objects 配列を持つ。音声パスを含まないので、アンカーだけを読む。
    if (j.contains("objects")) {
        try {
            return parse_tcmorph(j);
        } catch (const std::exception& e) {
            rethrow_with("tcmorph 形式のアンカーが不正: ", e);
        }
    }

    // 版（無ければ 1 とみなす）。新しい版のアプリで保存されたものは、知らない項目や意味の変わった
    // 項目があるかもしれないので読まない（黙って一部だけ読むと、保存し直したときに情報が消える）。
    if (const auto it = j.find("version"); it != j.end()) {
        if (!it->is_number_integer()) throw std::runtime_error("version が不正: 整数ではありません");
        const int v = it->get<int>();
        if (v > kSchemaVersion)
            throw std::runtime_error("新しい版のアプリで保存されたセッションです（version " + std::to_string(v)
                                     + "。このアプリが読めるのは " + std::to_string(kSchemaVersion) + " まで）");
    }

    SessionFile s;
    try {
        s.base_path   = j.at("waves").at("base").get<std::string>();
        s.target_path = j.at("waves").at("target").get<std::string>();
    } catch (const std::exception& e) {
        rethrow_with("waves が不正: ", e);
    }

    // 書き起こしは任意項目（無ければ空のまま）。
    if (const auto it = j.find("transcript"); it != j.end() && it->is_string()) s.transcript = it->get<std::string>();

    try {
        for (const json& ja : j.at("anchors")) {
            Anchor a;
            a.base_t   = ja.at("time").at("base").get<double>();
            a.target_t = ja.at("time").at("target").get<double>();
            for (const json& jf : ja.at("freqs"))
                a.freqs.push_back(FreqAnchor { jf.at("base").get<double>(), jf.at("target").get<double>() });
            s.anchors.push_back(std::move(a));
        }
    } catch (const std::exception& e) {
        rethrow_with("anchors が不正: ", e);
    }
    return s;
}

}    // namespace

Result<SessionFile> parse_session_json(const std::string& text) {
    try {
        return Result<SessionFile>::success(parse_session_json_or_throw(text));
    } catch (const std::exception& e) {
        return Result<SessionFile>::failure(e.what());
    }
}
