#include "session.hpp"

#include <exception>
#include <fstream>
#include <vector>

#include <nlohmann/json.hpp>

#include "app.hpp"
#include "log.hpp"

using json = nlohmann::json;

namespace {

constexpr int kSchemaVersion = 1;

bool file_readable(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

}    // namespace

bool save_session(const App& app, const std::string& path) {
    json j;
    j["version"] = kSchemaVersion;
    j["waves"]   = { { "base", app.base.path }, { "target", app.target.path } };

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

bool load_session(App& app, const std::string& path) {
    std::ifstream is(path);
    if (!is) {
        applog::add("セッション読み込み失敗: ファイルを開けません: " + path);
        return false;
    }

    json j;
    try {
        is >> j;
    } catch (const std::exception& e) {
        applog::add(std::string { "セッション読み込み失敗: JSON 解析エラー: " } + e.what());
        return false;
    }

    std::string base_path, target_path;
    try {
        base_path   = j.at("waves").at("base").get<std::string>();
        target_path = j.at("waves").at("target").get<std::string>();
    } catch (const std::exception& e) {
        applog::add(std::string { "セッション読み込み失敗: waves が不正: " } + e.what());
        return false;
    }

    // 音声ファイルの存在を先に確認（片方だけ復元して中途半端な状態になるのを防ぐ）。
    if (!file_readable(base_path)) {
        applog::add("セッション読み込み失敗: base 音声が見つかりません: " + base_path);
        return false;
    }
    if (!file_readable(target_path)) {
        applog::add("セッション読み込み失敗: target 音声が見つかりません: " + target_path);
        return false;
    }

    // アンカーを先にパースしておく（音声を差し替える前に検証を済ませる）。
    std::vector<Anchor> anchors;
    try {
        for (const json& ja : j.at("anchors")) {
            Anchor a;
            a.base_t   = ja.at("time").at("base").get<double>();
            a.target_t = ja.at("time").at("target").get<double>();
            for (const json& jf : ja.at("freqs"))
                a.freqs.push_back(FreqAnchor { jf.at("base").get<double>(), jf.at("target").get<double>() });
            anchors.push_back(std::move(a));
        }
    } catch (const std::exception& e) {
        applog::add(std::string { "セッション読み込み失敗: anchors が不正: " } + e.what());
        return false;
    }

    // 音声を復元（デコード失敗などはここで検出。詳細は load_track_from_path が applog に出す）。
    if (!load_track_from_path(app.base, base_path)) return false;
    if (!load_track_from_path(app.target, target_path)) return false;

    app.anchors = std::move(anchors);
    applog::add("セッション読み込みました: " + path);
    return true;
}
