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

    try {
        d.base_path   = j.at("waves").at("base").get<std::string>();
        d.target_path = j.at("waves").at("target").get<std::string>();
    } catch (const std::exception& e) {
        applog::add(std::string { "セッション読み込み失敗: waves が不正: " } + e.what());
        return d;
    }

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
    apply_track(app.base, d.base_path, std::move(d.base_spec));
    apply_track(app.target, d.target_path, std::move(d.target_spec));
    app.anchors = std::move(d.anchors);
}
