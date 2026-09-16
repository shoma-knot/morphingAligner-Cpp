// morph_generalized.cpp
//
// mat2json.py が変換した MorphObj を読み込み、C++ 版でモーフィングして
// 結果を npy に書き出す。MATLAB 側の出力とは compare_parity.py で比較する。
//
//   g++ -std=c++17 -O3 -march=native -fopenmp -I/usr/include/eigen3 \
//       test_matlab_parity.cpp -o test_matlab_parity
//
//   ./test_matlab_parity <変換先ディレクトリ> [alpha ...]
//
// 例:
//   python mat2json.py morphdBase.mat out/ --names speakerA speakerB
//   ./test_matlab_parity out/ 0.0 0.25 0.5 0.75 1.0
//   （MATLAB で export_reference.m を実行）
//   python compare_parity.py out/

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <tcmorph/anchor_io.hpp>
#include <tcmorph/world_io.hpp>

using namespace tcmorph;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <dir> [alpha ...]\n", argv[0]);
    return 1;
  }
  const std::string dir = argv[1];

  std::vector<double> alphas;
  for (int i = 2; i < argc; ++i) alphas.push_back(std::atof(argv[i]));
  if (alphas.empty()) alphas = {0.0, 0.25, 0.5, 0.75, 1.0};

  try {
    // WORLD パラメータとアンカーを別々に読み、名前で対応づける。
    // 警告は自分で集めて要約する（アンカーが多いと件数が増えるため）
    io::WorldSet ws = io::LoadWorldSet(dir + "/manifest.json");
    const io::AnchorSet as = io::LoadAnchorSet(dir + "/anchors.json", false);
    std::vector<std::string> warnings = as.warnings;
    io::ApplyAnchorSet(as, ws.names, &ws.objects, &warnings);

    const std::size_t n_obj = ws.objects.size();
    std::printf("素材 %zu 個を読み込みました\n", n_obj);
    for (std::size_t i = 0; i < n_obj; ++i) {
      const auto& wp = ws.objects[i].world_parameter;
      std::printf("  %-12s fs=%.0fHz  duration=%.4fs  frames=%lld  bins=%lld\n",
                  ws.names[i].c_str(), wp.sampling_frequency, wp.duration(),
                  static_cast<long long>(wp.spectrum_parameter.spectrogram.cols()),
                  static_cast<long long>(wp.spectrum_parameter.spectrogram.rows()));
    }

    if (!warnings.empty()) {
      const std::size_t show = std::min<std::size_t>(warnings.size(), 5);
      std::printf("\n警告 %zu 件（計算は続行します）:\n", warnings.size());
      for (std::size_t i = 0; i < show; ++i)
        std::printf("  * %s\n", warnings[i].c_str());
      if (warnings.size() > show)
        std::printf("  ... 他 %zu 件\n", warnings.size() - show);
    }

    // alpha を掃くのは 2 素材のときだけ。3 素材以上は等重みで 1 点だけ試す
    std::vector<VectorXd> weight_list;
    std::vector<std::string> tags;
    if (n_obj == 2) {
      for (double a : alphas) {
        VectorXd w(2);
        w << 1.0 - a, a;
        weight_list.push_back(w);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6g", a);
        tags.push_back(buf);
      }
    } else {
      weight_list.push_back(VectorXd::Constant(n_obj, 1.0 / n_obj));
      tags.push_back("uniform");
    }

    std::printf("\n%-8s %-10s %-12s %-12s %s\n", "alpha", "frames", "duration",
                "mean_fo", "time");
    for (std::size_t k = 0; k < weight_list.size(); ++k) {
      const VectorXd& w = weight_list[k];
      const MorphOutput out =
          GeneralizedTCMorphing(ws.objects, MorphWeights{w, w, w, w, w});

      for (const auto& m : out.warnings)
        std::printf("  ! alpha=%s: %s\n", tags[k].c_str(), m.c_str());

      const VectorXd& f0 = out.source_parameter.f0;
      const Index n_voiced = (f0.array() > 0).count();
      const double mean_fo =
          n_voiced > 0 ? (f0.array() > 0).select(f0, 0).sum() / n_voiced : 0.0;

      std::printf("%-8s %-10lld %-12.4f %-12.2f %.1f ms\n", tags[k].c_str(),
                  static_cast<long long>(f0.size()),
                  out.source_parameter.temporal_positions[f0.size() - 1], mean_fo,
                  out.elapsed_time * 1000.0);

      const std::string pre = dir + "/cpp_alpha" + tags[k] + "_";
      io::SaveNpy(pre + "spectrogram.npy", out.spectrum_parameter.spectrogram);
      io::SaveNpy(pre + "aperiodicity.npy", out.source_parameter.aperiodicity);
      io::SaveNpy(pre + "f0.npy", out.source_parameter.f0);
      io::SaveNpy(pre + "vuv.npy", out.source_parameter.vuv);
      io::SaveNpy(pre + "temporal_positions.npy",
                  out.source_parameter.temporal_positions);
    }
    std::printf("\n書き出し: %s/cpp_alpha*_*.npy\n", dir.c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "エラー: %s\n", e.what());
    return 1;
  }
  return 0;
}
