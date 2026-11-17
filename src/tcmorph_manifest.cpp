#include "tcmorph_manifest.hpp"

#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>

namespace {

namespace fs = std::filesystem;
using json   = nlohmann::json;

// manifest の objects[i] から音声のパスを決める（実在しなければ error）。
Result<std::string> resolve_audio(const json& obj, const fs::path& manifest_dir, const char* role) {
    const auto name_it = obj.find("fileName");
    if (name_it == obj.end() || !name_it->is_string() || name_it->get<std::string>().empty())
        return Result<std::string>::failure(std::string { "manifest.json の " } + role + " に fileName がありません");
    std::string dir;
    if (const auto it = obj.find("fileFullPath"); it != obj.end()) {
        if (!it->is_string())
            return Result<std::string>::failure(std::string { "manifest.json の " } + role + " の fileFullPath が文字列ではありません");
        dir = it->get<std::string>();
    }
    fs::path p = fs::u8path(dir) / fs::u8path(name_it->get<std::string>());
    if (p.is_relative()) p = manifest_dir / p;
    p = p.lexically_normal();
    std::error_code ec;
    if (!fs::is_regular_file(p, ec))
        return Result<std::string>::failure(std::string { "manifest.json の " } + role + " の音声が見つかりません: "
                                            + p.u8string()
                                            + "（場所が変わったなら --base / --target で指定してください）");
    return Result<std::string>::success(p.u8string());
}

// manifest の objects[i] の標本化周波数。
Result<double> sampling_frequency(const json& obj, const char* role) {
    const auto it = obj.find("sampling_frequency");
    if (it == obj.end() || !it->is_number() || !(it->get<double>() > 0))
        return Result<double>::failure(std::string { "manifest.json の " } + role + " に sampling_frequency がありません");
    return Result<double>::success(it->get<double>());
}

}    // namespace

Result<TcmorphManifest> read_tcmorph_manifest(const std::string& anchors_path) {
    const fs::path  dir  = fs::u8path(anchors_path).parent_path();
    const fs::path  path = dir / "manifest.json";
    TcmorphManifest m;
    m.path = path.u8string();

    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
        return Result<TcmorphManifest>::failure("tcmorph のアンカー JSON と同じフォルダに manifest.json がありません: " + m.path
                                                + "（--base と --target で音声を指定してください）");
    try {
        std::ifstream is(path, std::ios::binary);
        if (!is) return Result<TcmorphManifest>::failure("manifest.json を開けません: " + m.path);
        std::ostringstream ss;
        ss << is.rdbuf();
        const json root = json::parse(ss.str());
        if (!root.is_object()) return Result<TcmorphManifest>::failure("manifest.json がオブジェクトではありません: " + m.path);
        const auto objs = root.find("objects");
        if (objs == root.end() || !objs->is_array() || objs->size() != 2)
            return Result<TcmorphManifest>::failure("manifest.json の objects が base/target の2素材ではありません: " + m.path);

        const json& b = (*objs)[0];
        const json& t = (*objs)[1];
        if (!b.is_object() || !t.is_object())
            return Result<TcmorphManifest>::failure("manifest.json の objects の要素がオブジェクトではありません: " + m.path);
        const Result<std::string> bp = resolve_audio(b, dir, "base（objects[0]）");
        if (!bp.ok()) return Result<TcmorphManifest>::failure(bp.error);
        const Result<std::string> tp = resolve_audio(t, dir, "target（objects[1]）");
        if (!tp.ok()) return Result<TcmorphManifest>::failure(tp.error);
        const Result<double> bf = sampling_frequency(b, "base（objects[0]）");
        if (!bf.ok()) return Result<TcmorphManifest>::failure(bf.error);
        const Result<double> tf = sampling_frequency(t, "target（objects[1]）");
        if (!tf.ok()) return Result<TcmorphManifest>::failure(tf.error);
        m.base_path   = bp.value;
        m.target_path = tp.value;
        m.base_fs     = bf.value;
        m.target_fs   = tf.value;

        // 記録: objects（素材ごとの WORLD パラメータの情報）以外の項目をそのまま残す。
        json rec = json::object();
        for (const auto& [k, v] : root.items())
            if (k != "objects" && k != "version") rec[k] = v;
        rec["anchors"]  = fs::u8path(anchors_path).lexically_normal().u8string();
        rec["manifest"] = m.path;
        rec["names"]    = json::array({ b.value("name", ""), t.value("name", "") });
        m.record        = rec.dump();
    } catch (const std::exception& e) {
        return Result<TcmorphManifest>::failure("manifest.json を読めません: " + m.path + "（" + e.what() + "）");
    }
    return Result<TcmorphManifest>::success(std::move(m));
}

Status check_manifest_sample_rates(const TcmorphManifest& m, int base_fs_actual, int target_fs_actual) {
    for (const auto& [role, path, expected, actual] :
         { std::tuple { "base", &m.base_path, m.base_fs, base_fs_actual },
           std::tuple { "target", &m.target_path, m.target_fs, target_fs_actual } }) {
        if (std::abs(expected - actual) > 0.5)
            return Status::failure(std::string { role } + " の音声の標本化周波数が manifest.json と違います（manifest: "
                                   + std::to_string(static_cast<long long>(std::lround(expected))) + " Hz、音声: "
                                   + std::to_string(actual) + " Hz）: " + *path);
    }
    return {};
}
