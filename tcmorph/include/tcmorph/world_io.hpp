// tcmorph/world_io.hpp
//
// mat2json.py が書き出した manifest.json と npy から、MorphObject の
// world_parameter を復元する。アンカーは tc_morphing_anchor_io.hpp が扱う。
//
// MATLAB 版との数値比較が目的なので、C++ 側で WORLD 分析をやり直さず
// MATLAB 側の分析結果をそのまま読み込む。そうしないと差分が
// WORLD 実装由来なのかモーフィング実装由来なのか切り分けられない。
//
// ---------------------------------------------------------------------------
// npy の向きについて
// ---------------------------------------------------------------------------
// mat2json.py はスペクトログラムを shape (n_frame, n_fbin) の C 連続で書く。
// これは Eigen の MatrixXd(n_fbin, n_frame)（列優先）とメモリ配置が同一なので、
// 転置もループも要らず、そのまま memcpy できる。

#ifndef TCMORPH_WORLD_IO_HPP_
#define TCMORPH_WORLD_IO_HPP_

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <tcmorph/generalized_tc_morphing.hpp>

namespace tcmorph {
namespace io {

namespace npy {

/// npy ファイルから読み出した生データ
struct Array {
  std::vector<std::size_t> shape;
  bool fortran_order = false;
  std::vector<double> data;  ///< 型によらず double に昇格して保持する

  std::size_t size() const {
    std::size_t n = 1;
    for (std::size_t s : shape) n *= s;
    return n;
  }
};

namespace detail_npy {

inline void Fail(const std::string& m) {
  throw std::invalid_argument("tcmorph::io::npy: " + m);
}

/// ヘッダ辞書から "key: value" を雑に切り出す。
/// npy のヘッダは Python の dict リテラルだが、必要なのは 3 キーだけなので
/// 本格的なパーサは使わない。
inline std::string Field(const std::string& h, const std::string& key) {
  const std::string pat = "'" + key + "'";
  const std::size_t p = h.find(pat);
  if (p == std::string::npos) Fail("ヘッダに " + key + " がありません");
  std::size_t q = h.find(':', p);
  if (q == std::string::npos) Fail("ヘッダの " + key + " が壊れています");
  ++q;
  while (q < h.size() && (h[q] == ' ')) ++q;

  // 値は "(...)" か "'...'" か bare word のいずれか
  if (h[q] == '(') {
    const std::size_t e = h.find(')', q);
    return h.substr(q, e - q + 1);
  }
  if (h[q] == '\'') {
    const std::size_t e = h.find('\'', q + 1);
    return h.substr(q + 1, e - q - 1);
  }
  std::size_t e = q;
  while (e < h.size() && h[e] != ',' && h[e] != '}') ++e;
  std::string v = h.substr(q, e - q);
  while (!v.empty() && (v.back() == ' ')) v.pop_back();
  return v;
}

}  // namespace detail_npy

/// npy ファイルを読む。対応する dtype は <f8 / <f4 / <i8 / <i4。
inline Array Load(const std::string& path) {
  using detail_npy::Fail;
  using detail_npy::Field;

  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) Fail("ファイルを開けません: " + path);

  char magic[6];
  ifs.read(magic, 6);
  if (std::memcmp(magic, "\x93NUMPY", 6) != 0) Fail("npy ではありません: " + path);

  std::uint8_t major = 0, minor = 0;
  ifs.read(reinterpret_cast<char*>(&major), 1);
  ifs.read(reinterpret_cast<char*>(&minor), 1);

  std::size_t header_len = 0;
  if (major == 1) {
    std::uint16_t n = 0;
    ifs.read(reinterpret_cast<char*>(&n), 2);
    header_len = n;
  } else if (major == 2 || major == 3) {
    std::uint32_t n = 0;
    ifs.read(reinterpret_cast<char*>(&n), 4);
    header_len = n;
  } else {
    Fail("未対応の npy バージョン: " + std::to_string(major));
  }

  std::string header(header_len, '\0');
  ifs.read(&header[0], static_cast<std::streamsize>(header_len));

  Array a;
  const std::string descr = Field(header, "descr");
  a.fortran_order = (Field(header, "fortran_order") == "True");

  // shape は "(3, 4)" や "(5,)" の形
  {
    const std::string s = Field(header, "shape");
    std::string num;
    for (char c : s) {
      if (c >= '0' && c <= '9') {
        num += c;
      } else if (!num.empty()) {
        a.shape.push_back(static_cast<std::size_t>(std::stoull(num)));
        num.clear();
      }
    }
    if (!num.empty()) a.shape.push_back(static_cast<std::size_t>(std::stoull(num)));
  }

  const std::size_t n = a.size();
  a.data.resize(n);

  // 型ごとに読み込んで double に昇格する
  auto read_as = [&](auto tag) {
    using T = decltype(tag);
    std::vector<T> buf(n);
    ifs.read(reinterpret_cast<char*>(buf.data()),
             static_cast<std::streamsize>(n * sizeof(T)));
    if (!ifs) Fail("データが途中で終わっています: " + path);
    for (std::size_t i = 0; i < n; ++i) a.data[i] = static_cast<double>(buf[i]);
  };

  if (descr == "<f8" || descr == "f8") read_as(double{});
  else if (descr == "<f4" || descr == "f4") read_as(float{});
  else if (descr == "<i8" || descr == "i8") read_as(std::int64_t{});
  else if (descr == "<i4" || descr == "i4") read_as(std::int32_t{});
  else Fail("未対応の dtype: " + descr + " (" + path + ")");

  return a;
}

/// 1 次元 npy を VectorXd として読む
inline VectorXd LoadVector(const std::string& path, Index expect = -1) {
  const Array a = Load(path);
  if (a.shape.size() != 1)
    detail_npy::Fail("1 次元配列を期待しましたが " +
                     std::to_string(a.shape.size()) + " 次元でした: " + path);
  if (expect >= 0 && static_cast<Index>(a.size()) != expect)
    detail_npy::Fail("要素数 " + std::to_string(a.size()) + " が期待値 " +
                     std::to_string(expect) + " と異なります: " + path);
  return Eigen::Map<const VectorXd>(a.data.data(), static_cast<Index>(a.size()));
}

/// (n_frame, n_fbin) の C 連続 npy を MatrixXd(n_fbin, n_frame) として読む。
/// メモリ配置が列優先とそのまま一致するので、詰め替えは Map の一発で済む。
inline MatrixXd LoadSpectrogram(const std::string& path, Index n_fbin,
                                Index n_frame) {
  const Array a = Load(path);
  if (a.shape.size() != 2)
    detail_npy::Fail("2 次元配列を期待しました: " + path);
  if (a.fortran_order)
    detail_npy::Fail("C 連続の npy を期待しました（mat2json.py が生成したもの "
                     "以外を渡していませんか）: " + path);
  if (static_cast<Index>(a.shape[0]) != n_frame ||
      static_cast<Index>(a.shape[1]) != n_fbin)
    detail_npy::Fail("shape (" + std::to_string(a.shape[0]) + ", " +
                     std::to_string(a.shape[1]) + ") が期待値 (" +
                     std::to_string(n_frame) + ", " + std::to_string(n_fbin) +
                     ") と異なります: " + path);
  return Eigen::Map<const MatrixXd>(a.data.data(), n_fbin, n_frame);
}

}  // namespace npy

// ===========================================================================
// manifest.json から MorphObject を組み立てる
// ===========================================================================

/// 読み込み結果。名前は anchors.json との対応づけに使う
struct WorldSet {
  std::vector<MorphObject> objects;
  std::vector<std::string> names;
};

inline WorldSet LoadWorldSet(const std::string& manifest_path) {
  std::ifstream ifs(manifest_path);
  if (!ifs)
    throw std::invalid_argument("tcmorph::io: manifest を開けません: " +
                                manifest_path);
  nlohmann::json root;
  ifs >> root;

  // npy の相対パスは manifest からの相対として解決する
  std::string base = manifest_path;
  const std::size_t slash = base.find_last_of("/\\");
  base = (slash == std::string::npos) ? std::string(".") : base.substr(0, slash);

  WorldSet set;
  for (const auto& jo : root.at("objects")) {
    const std::string name = jo.at("name").get<std::string>();
    const Index n_fbin = jo.at("n_fbin").get<Index>();
    const Index n_src = jo.at("n_source_frames").get<Index>();
    const Index n_spc = jo.at("n_spectrum_frames").get<Index>();
    const auto& files = jo.at("files");

    auto path_of = [&](const char* key) {
      return base + "/" + files.at(key).get<std::string>();
    };

    MorphObject obj;
    auto& wp = obj.world_parameter;
    wp.sampling_frequency = jo.at("sampling_frequency").get<double>();
    wp.span_length = jo.at("span_length").get<std::size_t>();

    wp.source_parameter.temporal_positions =
        npy::LoadVector(path_of("source_temporal_positions"), n_src);
    wp.source_parameter.f0 = npy::LoadVector(path_of("f0"), n_src);
    // wordTV2WmorphingEngineRev 用。無ければ f0 で代用する
    wp.f0_original = files.contains("f0_original")
                         ? npy::LoadVector(path_of("f0_original"), n_src)
                         : wp.source_parameter.f0;
    wp.source_parameter.vuv = npy::LoadVector(path_of("vuv"), n_src);
    wp.source_parameter.aperiodicity =
        npy::LoadSpectrogram(path_of("aperiodicity"), n_fbin, n_src);

    wp.spectrum_parameter.temporal_positions =
        npy::LoadVector(path_of("spectrum_temporal_positions"), n_spc);
    wp.spectrum_parameter.spectrogram =
        npy::LoadSpectrogram(path_of("spectrogram"), n_fbin, n_spc);
    wp.spectrum_parameter.fs = wp.sampling_frequency;

    set.objects.push_back(std::move(obj));
    set.names.push_back(name);
  }
  return set;
}

// ===========================================================================
// 比較用の書き出し
// ===========================================================================

/// MatrixXd(n_fbin, n_frame) を (n_frame, n_fbin) の C 連続 npy として書く。
/// 列優先のメモリをそのまま流せる
inline void SaveNpy(const std::string& path, const MatrixXd& m) {
  std::ofstream ofs(path, std::ios::binary);
  std::string header = "{'descr': '<f8', 'fortran_order': False, 'shape': (" +
                       std::to_string(m.cols()) + ", " + std::to_string(m.rows()) +
                       "), }";
  while ((10 + header.size() + 1) % 64 != 0) header += ' ';
  header += '\n';
  const std::uint16_t hlen = static_cast<std::uint16_t>(header.size());

  ofs.write("\x93NUMPY", 6);
  ofs.put(1).put(0);
  ofs.write(reinterpret_cast<const char*>(&hlen), 2);
  ofs.write(header.data(), static_cast<std::streamsize>(header.size()));
  ofs.write(reinterpret_cast<const char*>(m.data()),
            static_cast<std::streamsize>(m.size() * sizeof(double)));
}

inline void SaveNpy(const std::string& path, const VectorXd& v) {
  std::ofstream ofs(path, std::ios::binary);
  std::string header = "{'descr': '<f8', 'fortran_order': False, 'shape': (" +
                       std::to_string(v.size()) + ",), }";
  while ((10 + header.size() + 1) % 64 != 0) header += ' ';
  header += '\n';
  const std::uint16_t hlen = static_cast<std::uint16_t>(header.size());

  ofs.write("\x93NUMPY", 6);
  ofs.put(1).put(0);
  ofs.write(reinterpret_cast<const char*>(&hlen), 2);
  ofs.write(header.data(), static_cast<std::streamsize>(header.size()));
  ofs.write(reinterpret_cast<const char*>(v.data()),
            static_cast<std::streamsize>(v.size() * sizeof(double)));
}

}  // namespace io
}  // namespace tcmorph

#endif  // TCMORPH_WORLD_IO_HPP_
