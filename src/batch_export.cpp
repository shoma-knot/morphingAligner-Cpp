#include "batch_export.hpp"

#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "morphing.hpp"
#include "session_io.hpp"

namespace {

// ファイル名に使えない文字（Windows の禁止文字とパス区切り）。接頭辞に混ざると
// 意図しない場所へ書いてしまうので弾く。
constexpr const char* kForbidden = R"(/\:*?"<>|)";

// 出力先のファイルのパス（UTF-8）。
std::string out_path(const std::string& dir, const std::string& name) {
    return (std::filesystem::u8path(dir) / std::filesystem::u8path(name)).u8string();
}

// BatchExportSettings::sweep の既定値（全軸）が軸の数と合っていること。
static_assert(kMorphAxisCount == 5, "BatchExportSettings::sweep の初期値を軸の数に合わせること");

// ファイル名に入れる軸の名前（全軸なら空、選んだ軸だけなら "_fo+sl"）。
std::string axes_tag(const BatchExportSettings& s) {
    if (batch_sweeps_all_axes(s)) return {};
    std::string tag;
    for (int i = 0; i < kMorphAxisCount; ++i) {
        if (!s.sweep[i]) continue;
        tag += tag.empty() ? "_" : "+";
        tag += kMorphAxes[i].key;
    }
    return tag;
}

// 各軸の率を JSON のオブジェクトにする（{"tx": 0.5, ...}）。
nlohmann::json rates_json(const MorphRates& r) {
    nlohmann::json j = nlohmann::json::object();
    for (const MorphAxis& a : kMorphAxes) j[a.key] = r.*a.rate;
    return j;
}

}    // namespace

std::vector<double> batch_rates(int count) {
    std::vector<double> r;
    if (count < 2) return r;
    r.reserve(static_cast<std::size_t>(count));
    // 両端をちょうど 0 と 1 にするため、刻みを足していくのではなく毎回割り算する。
    for (int i = 0; i < count; ++i) r.push_back(static_cast<double>(i) / (count - 1));
    return r;
}

bool batch_sweeps_all_axes(const BatchExportSettings& s) {
    for (bool b : s.sweep)
        if (!b) return false;
    return true;
}

MorphRates batch_point_rates(const BatchExportSettings& s, double rate) {
    MorphRates r = s.fixed;
    for (int i = 0; i < kMorphAxisCount; ++i)
        if (s.sweep[i]) r.*kMorphAxes[i].rate = rate;
    return r;
}

std::string batch_wav_name(const BatchExportSettings& s, double rate) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "_r%03d.wav", static_cast<int>(std::lround(rate * 100.0)));
    return s.prefix + axes_tag(s) + buf;
}

std::string batch_manifest_name(const BatchExportSettings& s) {
    return s.prefix + axes_tag(s) + "_batch.json";
}

Status validate_batch_settings(const BatchExportSettings& s) {
    if (s.count < BatchExportSettings::kCountMin || s.count > BatchExportSettings::kCountMax)
        return Status::failure("本数は " + std::to_string(BatchExportSettings::kCountMin) + "〜"
                               + std::to_string(BatchExportSettings::kCountMax) + " で指定してください");
    bool any = false;
    for (int i = 0; i < kMorphAxisCount; ++i) {
        if (s.sweep[i]) {
            any = true;
            continue;
        }
        const double v = s.fixed.*kMorphAxes[i].rate;
        if (!(v >= 0.0 && v <= 1.0))
            return Status::failure(std::string { "固定する率は 0〜1 で指定してください（" } + kMorphAxes[i].key + "）");
    }
    if (!any) return Status::failure("変化させる軸を1つ以上選んでください");
    if (s.prefix.empty()) return Status::failure("ファイル名の接頭辞を入力してください");
    if (s.prefix.find_first_of(kForbidden) != std::string::npos)
        return Status::failure(R"(接頭辞に使えない文字が含まれています（ / \ : * ? " < > | ）)");
    if (s.dir.empty()) return Status::failure("出力先フォルダを選んでください");
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::u8path(s.dir), ec))
        return Status::failure("出力先がフォルダではありません: " + s.dir);
    return {};
}

int count_existing_batch_files(const BatchExportSettings& s) {
    if (s.dir.empty() || s.prefix.empty()) return 0;
    std::error_code ec;
    int             n = 0;
    for (double rate : batch_rates(s.count))
        if (std::filesystem::exists(std::filesystem::u8path(out_path(s.dir, batch_wav_name(s, rate))), ec)) ++n;
    if (s.write_manifest && std::filesystem::exists(std::filesystem::u8path(out_path(s.dir, batch_manifest_name(s))), ec))
        ++n;
    return n;
}

BatchExportResult run_batch_export(const BatchExportRequest& req, BatchExportProgress& progress) {
    BatchExportResult          R;
    const BatchExportSettings& s = req.settings;

    if (const Status v = validate_batch_settings(s); !v.ok()) {
        R.error = v.error;
        return R;
    }
    if (!req.base || !req.target) {
        R.error = "base/target の解析データがありません";
        return R;
    }

    nlohmann::json files = nlohmann::json::array();
    for (double rate : batch_rates(s.count)) {
        if (progress.cancel.load()) {
            R.canceled = true;
            break;
        }

        // 1本ぶんの結果は morphed の f0/sp/ap も抱えていて大きい。ここで寿命を切り、
        // 書き出したら捨てる（全本ぶん溜めると簡単に GB 級になる）。
        const MorphRates  rates = batch_point_rates(s, rate);
        const MorphOutput o     = morphing_channels(*req.base, *req.target, req.anchors, rates);
        if (!o.ok()) {
            R.error = o.error;
            return R;
        }
        // 警告はアンカー由来なので率によらず同じ内容になる。1本目のものだけ持ち帰る。
        if (R.written == 0) R.warnings = o.warnings;

        const std::string name = batch_wav_name(s, rate);
        if (const Status st = write_wav(out_path(s.dir, name), o.wave, o.fs); !st.ok()) {
            R.error = "WAV 保存失敗: " + st.error;
            return R;
        }

        files.push_back({ { "file", name }, { "rate", rate }, { "rates", rates_json(rates) } });
        ++R.written;
        progress.done.store(R.written);
    }

    // 何をどの率で書いたかを残す。中身はセッション形式に batch の項目を足しただけなので、
    // この JSON はそのまま「セッション読み込み」で開ける（知らない項目は読み飛ばされる）。
    if (s.write_manifest && R.written > 0) {
        try {
            SessionFile sf;
            sf.base_path   = req.base_path;
            sf.target_path = req.target_path;
            sf.transcript  = req.transcript;
            sf.anchors     = req.anchors;

            nlohmann::json j = nlohmann::json::parse(session_to_json(sf));
            // sweep は掃引した軸の名前、fixed は掃引しなかった軸の率（全軸なら空）。
            nlohmann::json sweep = nlohmann::json::array(), fixed = nlohmann::json::object();
            for (int i = 0; i < kMorphAxisCount; ++i) {
                if (s.sweep[i]) sweep.push_back(kMorphAxes[i].key);
                else fixed[kMorphAxes[i].key] = s.fixed.*kMorphAxes[i].rate;
            }
            j["batch"] = { { "count", s.count },           { "sweep", std::move(sweep) },
                           { "fixed", std::move(fixed) },  { "canceled", R.canceled },
                           { "files", std::move(files) } };

            const std::string path = out_path(s.dir, batch_manifest_name(s));
            std::ofstream     os(std::filesystem::u8path(path), std::ios::binary);
            if (!os) {
                R.error = "設定の JSON を書けません: " + path;
                return R;
            }
            os << j.dump(2) << '\n';
            R.manifest = path;
        } catch (const std::exception& e) {
            R.error = std::string { "設定の JSON を書けません: " } + e.what();
            return R;
        }
    }
    return R;
}
