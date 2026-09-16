// morph_aligner.cpp
//
// morphingAligner / morphingSoundGenerator が使うエンジン
// (wordTV2WmorphingEngineRev) で、mat2json.py が変換した edit.mat を
// モーフィングして npy に書き出す。
//
//   g++ -std=c++17 -O3 -march=native -fopenmp -I/usr/include/eigen3 -Isrc \
//       src/test_aligner_parity.cpp -o test_aligner_parity
//
//   ./test_aligner_parity <変換先ディレクトリ> [mRate ...]
//
// mRate は GUI の mRateSlider の値。0 が参照、1 が目標。
// 省略すると manifest.json の suggested_alpha（= testMrate）と 0, 0.5, 1 を使う。
//
// 既定では MATLAB 版の不具合もそのまま再現する。--fix を付けると修正版になり、
// 両者を比べることで不具合がどれだけ出音に効いているかを確認できる。

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <tcmorph/anchor_io.hpp>
#include <tcmorph/world_io.hpp>
#include <tcmorph/word_tv2w_morphing.hpp>

using namespace tcmorph;

namespace {

/// anchors.json の 1 素材分を (時間アンカー, 周波数アンカー) に取り出す
void PickAnchors(const io::AnchorSet& set, const std::string& name,
                 VectorXd* t, MatrixXd* tf) {
  const auto& o = set.at(name);
  *t = o.time_anchor;
  *tf = o.time_freq_anchor;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <dir> [--fix] [mRate ...]\n", argv[0]);
    return 1;
  }
  const std::string dir = argv[1];

  bool fix = false;
  std::vector<double> rates;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--fix") == 0) fix = true;
    else rates.push_back(std::atof(argv[i]));
  }

  try {
    io::WorldSet ws = io::LoadWorldSet(dir + "/manifest.json");
    if (ws.objects.size() != 2)
      throw std::invalid_argument(
          "このエンジンは参照と目標の 2 素材専用です（読み込んだのは " +
          std::to_string(ws.objects.size()) + " 個）");

    const io::AnchorSet as = io::LoadAnchorSet(dir + "/anchors.json", false);
    for (const auto& w : as.warnings) std::printf("警告: %s\n", w.c_str());

    // manifest から vtl_ratio と testMrate を拾う
    double vtl = 1.0, suggested = -1.0;
    {
      std::ifstream ifs(dir + "/manifest.json");
      nlohmann::json j;
      ifs >> j;
      if (j.contains("vtl_ratio")) vtl = j["vtl_ratio"].get<double>();
      if (j.contains("suggested_alpha")) suggested = j["suggested_alpha"].get<double>();
    }
    if (rates.empty()) {
      if (suggested >= 0.0) rates.push_back(suggested);
      rates.insert(rates.end(), {0.0, 0.5, 1.0});
    }

    VectorXd t_ref, t_tgt;
    MatrixXd tf_ref, tf_tgt;
    PickAnchors(as, ws.names[0], &t_ref, &tf_ref);
    PickAnchors(as, ws.names[1], &t_tgt, &tf_tgt);

    std::printf("エンジン: wordTV2WmorphingEngineRev%s\n",
                fix ? "（修正版）" : "（MATLAB の挙動を再現）");
    std::printf("参照=%s  目標=%s  vtl_ratio=%.4f\n",
                ws.names[0].c_str(), ws.names[1].c_str(), vtl);
    for (std::size_t i = 0; i < 2; ++i) {
      const auto& wp = ws.objects[i].world_parameter;
      std::printf("  %-12s fs=%.0fHz  frames=%lld  bins=%lld  末尾フレーム=%.4fs\n",
                  ws.names[i].c_str(), wp.sampling_frequency,
                  static_cast<long long>(wp.spectrum_parameter.spectrogram.cols()),
                  static_cast<long long>(wp.spectrum_parameter.spectrogram.rows()),
                  wp.source_parameter.temporal_positions
                      [wp.source_parameter.temporal_positions.size() - 1]);
    }

    aligner::Options opt;
    opt.vtl_ratio = vtl;
    if (fix) {
      opt.fix_segment_end_index = true;
      opt.emulate_catch_dropout = false;
    }

    std::printf("\n%-9s %-8s %-10s %-11s %s\n", "mRate", "frames", "duration",
                "mean_fo", "time");
    for (double r : rates) {
      const aligner::Output o = aligner::WordTV2WMorphing(
          ws.objects[0].world_parameter, ws.objects[1].world_parameter,
          t_ref, tf_ref, t_tgt, tf_tgt, aligner::MorphRate::Uniform(r), opt);

      char tag[32];
      std::snprintf(tag, sizeof(tag), "%.6g", r);
      std::printf("%-9s %-8lld %-10.4f %-11.2f %.1f ms\n", tag,
                  static_cast<long long>(o.f0.size()),
                  o.temporal_positions[o.f0.size() - 1], o.f0.mean(),
                  o.elapsed_time * 1000.0);
      for (const auto& w : o.warnings) std::printf("  ! %s\n", w.c_str());

      const std::string pre = dir + "/cpp_alpha" + tag + "_";
      io::SaveNpy(pre + "spectrogram.npy", o.spectrogram);
      io::SaveNpy(pre + "aperiodicity.npy", o.aperiodicity);
      io::SaveNpy(pre + "f0.npy", o.f0);
      io::SaveNpy(pre + "vuv.npy", o.vuv);
      io::SaveNpy(pre + "temporal_positions.npy", o.temporal_positions);
    }
    std::printf("\n書き出し: %s/cpp_alpha*_*.npy\n", dir.c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "エラー: %s\n", e.what());
    return 1;
  }
  return 0;
}
