// speech_tools（C++ から Python の音声解析ツールを子プロセスで呼ぶ部分）の結合テスト。
//
// アプリと同じ経路（子プロセスの起動・JSON の要求/応答・一時ディレクトリの後始末）で、
// 環境チェックとフォルマント推定を実際に走らせる。Windows の CreateProcessW と Linux の
// posix_spawn の両方をこれで確かめる（CI の ci.yml が各 OS で実行する）。
//
// 実行はリポジトリ（または配布物）のルートで行う。Python は ./.env（または環境変数
// MORPHALIGNER_PYTHON）、スクリプトは ./python/speech_tools.py を探すため。
// 音素アライメントは実際の発話が要るので対象外（環境チェックが MFA の起動と日本語の
// 単語分割までは確かめる）。
//
// ビルド: cmake -DMORPHALIGNER_BUILD_TESTS=ON ... → bin/speech_tools_test

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "speech_tools.hpp"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void expect(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? "OK" : "NG", what.c_str());
    if (!ok) ++g_failures;
}

// 16bit PCM モノラルの WAV を書く（テスト用の最小実装）。
void write_wav16(const fs::path& path, const std::vector<double>& x, int fs) {
    const auto put32 = [](std::ofstream& o, std::uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); };
    const auto put16 = [](std::ofstream& o, std::uint16_t v) { o.write(reinterpret_cast<const char*>(&v), 2); };
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(x.size() * 2);
    std::ofstream       o(path, std::ios::binary);
    o.write("RIFF", 4);
    put32(o, 36 + data_bytes);
    o.write("WAVEfmt ", 8);
    put32(o, 16);
    put16(o, 1);    // PCM
    put16(o, 1);    // モノラル
    put32(o, static_cast<std::uint32_t>(fs));
    put32(o, static_cast<std::uint32_t>(fs * 2));
    put16(o, 2);
    put16(o, 16);
    o.write("data", 4);
    put32(o, data_bytes);
    for (double v : x) put16(o, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(v * 30000.0))));
}

// 母音らしい合成音（120 Hz の倍音列を、700/1200/2600 Hz 付近を強めて重ねたもの）。
std::vector<double> synth_vowel(int fs, double seconds) {
    const double kPi   = 3.14159265358979323846;
    const double f0    = 120.0;
    const double fmt[] = { 700.0, 1200.0, 2600.0 };
    std::vector<double> x(static_cast<std::size_t>(fs * seconds), 0.0);
    double              peak = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        double s = 0.0;
        for (int k = 1; k * f0 < fs / 2.0; ++k) {
            double amp = 0.0;
            for (double f : fmt) amp += 1.0 / (1.0 + std::pow((k * f0 - f) / 80.0, 2.0));
            s += amp * std::sin(2.0 * kPi * k * f0 * static_cast<double>(n) / fs);
        }
        x[n] = s;
        peak = std::max(peak, std::fabs(s));
    }
    for (double& v : x) v /= peak;
    return x;
}

}    // namespace

int main() {
    std::printf("python: %s\nscript: %s\n", find_python().c_str(), find_speech_script().c_str());

    // 環境チェック（子プロセス起動・JSON のやりとり・MFA の起動・sudachi・モデルの有無）。
    const SpeechEnvStatus st = run_check(AlignParams {});
    for (const std::string& p : st.problems) std::printf("  problem: %s\n", p.c_str());
    for (const std::string& w : st.warnings) std::printf("  warning: %s\n", w.c_str());
    std::printf("  info: %s\n", st.summary.c_str());
    expect(st.ready, "環境チェックが ready を返す");

    // フォルマント推定（ASCII 以外を含むファイル名で、UTF-8 のパスが子プロセスまで届くことも見る）。
    const fs::path wav = fs::temp_directory_path() / fs::u8path(u8"morphaligner_テスト音声.wav");
    write_wav16(wav, synth_vowel(16000, 0.5), 16000);
    const Result<Formants> r = run_formants(wav.u8string(), FormantParams {});
    const Formants&        f = r.value;
    std::error_code        ec;
    fs::remove(wav, ec);
    expect(r.ok(), "フォルマント推定がエラーを返さない" + (r.ok() ? "" : "（" + r.error + "）"));
    expect(!f.tracks.empty() && !f.tracks[0].t.empty(), "F1 の推定値が得られる");
    if (!f.tracks.empty() && !f.tracks[0].hz.empty())
        std::printf("  F1 の最初の値: %.0f Hz（%zu 点）\n", f.tracks[0].hz.front(), f.tracks[0].hz.size());

    std::printf("%s\n", g_failures == 0 ? "すべて成功" : "失敗あり");
    return g_failures == 0 ? 0 : 1;
}
