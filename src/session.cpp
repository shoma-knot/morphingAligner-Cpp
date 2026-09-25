#include "session.hpp"

#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "app.hpp"
#include "log.hpp"
#include "session_io.hpp"

namespace {

bool file_readable(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

}    // namespace

bool save_session(const App& app, const std::string& path) {
    SessionFile s;
    s.base_path   = app.base.path;
    s.target_path = app.target.path;
    s.transcript  = app.speech.transcript;
    s.anchors     = app.anchors;

    std::ofstream os(path, std::ios::binary);
    if (!os) {
        applog::add("セッション保存失敗: ファイルを開けません: " + path);
        return false;
    }
    os << session_to_json(s);
    applog::add("セッション保存しました: " + path);
    return true;
}

SessionLoadData load_session_data(const std::string& path) {
    SessionLoadData d;

    std::string text;
    {
        std::ifstream is(path, std::ios::binary);
        if (!is) {
            applog::add("セッション読み込み失敗: ファイルを開けません: " + path);
            return d;
        }
        std::ostringstream ss;
        ss << is.rdbuf();
        text = ss.str();
    }

    // JSON の解釈（形式の検証を含む）。音声を差し替える前に済ませる。
    SessionFile s;
    try {
        s = parse_session_json(text);
    } catch (const std::exception& e) {
        applog::add(std::string { "セッション読み込み失敗: " } + e.what());
        return d;
    }
    for (const std::string& n : s.notes) applog::add(n);
    d.anchors = std::move(s.anchors);

    // tcmorph 形式は音声を含まないので、アンカーだけを現在の base/target に乗せる（apply_session_data）。
    if (s.anchors_only) {
        applog::add("アンカーを読み込みました: " + path);
        d.anchors_only = true;
        d.ok           = true;
        return d;
    }
    d.base_path   = std::move(s.base_path);
    d.target_path = std::move(s.target_path);
    d.transcript  = std::move(s.transcript);

    // 音声ファイルの存在を先に確認（片方だけ復元して中途半端な状態になるのを防ぐ）。
    if (!file_readable(d.base_path)) {
        applog::add("セッション読み込み失敗: base 音声が見つかりません: " + d.base_path);
        return d;
    }
    if (!file_readable(d.target_path)) {
        applog::add("セッション読み込み失敗: target 音声が見つかりません: " + d.target_path);
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
    app.speech.transcript = std::move(d.transcript);
}
