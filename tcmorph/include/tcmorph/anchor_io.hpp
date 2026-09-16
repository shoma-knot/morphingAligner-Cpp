// tcmorph/anchor_io.hpp
//
// generalized_tc_morphing_eigen.hpp の MorphObject のうち、
// time_anchor と time_freq_anchor だけを外部 JSON から読み書きする。
//
// WORLD パラメータは分析で機械的に決まるのに対し、アンカーは GUI で
// 人が手で打つ値なので、そこだけを外部ファイルに持たせる想定。
//
// ビルド例:
//   g++ -std=c++17 -O3 -fopenmp -I/usr/include/eigen3 ...
//   （nlohmann/json はヘッダオンリー。Ubuntu なら nlohmann-json3-dev）
//
// ---------------------------------------------------------------------------
// JSON 形式
// ---------------------------------------------------------------------------
// {
//   "version": 1,
//   "objects": [
//     {
//       "name": "speakerA",
//       "time_anchor": [0.20, 0.50, 0.75],
//       "time_freq_anchor": [
//         [700, 1200, 2600],      <- time_anchor[0] = 0.20 s における周波数アンカー
//         [720, 1250, 2650],      <- time_anchor[1] = 0.50 s
//         [690, 1180, 2580]       <- time_anchor[2] = 0.75 s
//       ]
//     },
//     { "name": "speakerB", ... }
//   ]
// }
//
// * time_freq_anchor の外側の添字は時間アンカー番号。MorphObject が持つ
//   Eigen 行列は (周波数アンカー, 時間アンカー) の転置した向きなので、
//   読み込み時に転置する。JSON 側を時刻ごとの行にしてあるのは、
//   time_anchor と並びが揃っていて人間が編集しやすいため。
// * 各行の長さは揃っていなくてよい（時刻によってアンカー本数が違う場合）。
//   短い行は 0 で自動的に詰められる。
// * time_freq_anchor を省略すると、その素材は周波数ワープなし（恒等写像）
//   として扱われる。
//
// ---------------------------------------------------------------------------
// 読み込み時に検証する内容
// ---------------------------------------------------------------------------
// エラー（計算が成立しないもの）:
//  * time_anchor が正かつ狭義単調増加であること
//    （区間長が 0 以下だと log を取れず NaN になる）
//  * time_freq_anchor の行数が time_anchor の長さと一致すること
//  * 各行の周波数が正であること（log を取るため）
//  * 全素材でアンカー本数が揃っていること
//  * (ApplyAnchorSet 時) 時間が発話長未満であること
//
// 警告（計算は成立するが、意図しない結果かもしれないもの）:
//  * 各行の周波数が狭義単調増加でない
//    素材ごとのアンカーは interp1 の y 側に入るので、逆転していても
//    折り返すワープ関数になるだけで計算は成立する。MATLAB 版も同じ。
//    実際に単調性が要求されるのはモーフィング後のアンカー位置（x 側）で、
//    そちらは素材間の重み付き和なので片方の逆転は打ち消されうる。
//    この検査は GeneralizedTCMorphing の側で行う。
//  * 周波数が Nyquist 以上
//
// 警告は AnchorSet::warnings に溜まる。既定では stderr にも出る。

#ifndef TCMORPH_ANCHOR_IO_HPP_
#define TCMORPH_ANCHOR_IO_HPP_

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <tcmorph/generalized_tc_morphing.hpp>

namespace tcmorph {
namespace io {

/// 1 素材分のアンカー
struct ObjectAnchors {
  std::string name;               ///< 任意。省略時は "object_<index>"
  VectorXd time_anchor;           ///< 時間アンカー [s]
  MatrixXd time_freq_anchor;      ///< (max_n_fanchor, n_tanchor) 周波数アンカー [Hz]
  ///< MorphObject と同じ向き（JSON からは転置して格納済み）
};

/// JSON ファイル 1 つ分
struct AnchorSet {
  int version = 1;
  std::vector<ObjectAnchors> objects;
  std::vector<std::string> warnings;  ///< 計算は成立するが注意が要る点

  std::size_t size() const { return objects.size(); }

  /// 名前で検索する。見つからなければ例外
  const ObjectAnchors& at(const std::string& name) const {
    for (const auto& o : objects)
      if (o.name == name) return o;
    throw std::invalid_argument("tcmorph::io: 名前 '" + name +
                                "' の素材が JSON にありません");
  }
};

namespace detail_io {

using nlohmann::json;

inline void Fail(const std::string& msg) {
  throw std::invalid_argument("tcmorph::io: " + msg);
}

/// JSON 配列を VectorXd に変換する
inline VectorXd ToVector(const json& j, const std::string& where) {
  if (!j.is_array()) Fail(where + " は配列である必要があります");
  VectorXd v(static_cast<Index>(j.size()));
  for (std::size_t i = 0; i < j.size(); ++i) {
    if (!j[i].is_number())
      Fail(where + "[" + std::to_string(i) + "] が数値ではありません");
    v[static_cast<Index>(i)] = j[i].get<double>();
  }
  return v;
}

/// 全要素が正であることを確認する（log を取るため必須）
inline void RequirePositive(const VectorXd& v, const std::string& where) {
  for (Index i = 0; i < v.size(); ++i)
    if (!(v[i] > 0.0))
      Fail(where + "[" + std::to_string(i) + "] = " + std::to_string(v[i]) +
           " が正ではありません");
}

/// 狭義単調増加かつ全要素が正であることを確認する
inline void RequireIncreasingPositive(const VectorXd& v, const std::string& where) {
  RequirePositive(v, where);
  for (Index i = 1; i < v.size(); ++i)
    if (!(v[i] > v[i - 1]))
      Fail(where + " が狭義単調増加ではありません（" + std::to_string(i - 1) +
           " 番目 " + std::to_string(v[i - 1]) + " -> " + std::to_string(i) +
           " 番目 " + std::to_string(v[i]) + "）");
}

/// 狭義単調増加かどうかを調べ、そうでなければ警告文を返す。例外は投げない
inline bool CheckIncreasing(const VectorXd& v, const std::string& where,
                            std::string* msg) {
  for (Index i = 1; i < v.size(); ++i)
    if (!(v[i] > v[i - 1])) {
      *msg = where + " が狭義単調増加ではありません（" + std::to_string(i - 1) +
             " 番目 " + std::to_string(v[i - 1]) + " -> " + std::to_string(i) +
             " 番目 " + std::to_string(v[i]) + "）。" +
             "折り返すワープ関数になりますが計算は成立します";
      return false;
    }
  return true;
}

}  // namespace detail_io

// ===========================================================================
// 読み込み
// ===========================================================================

/// パース済み JSON から AnchorSet を構築する
inline AnchorSet ParseAnchorSet(const nlohmann::json& root) {
  using detail_io::Fail;
  using detail_io::ToVector;
  using detail_io::RequireIncreasingPositive;

  AnchorSet set;
  if (root.contains("version")) set.version = root.at("version").get<int>();
  if (set.version != 1)
    Fail("未対応の version です: " + std::to_string(set.version));

  if (!root.contains("objects") || !root.at("objects").is_array())
    Fail("トップレベルに配列 \"objects\" が必要です");

  const auto& arr = root.at("objects");
  if (arr.empty()) Fail("\"objects\" が空です");

  for (std::size_t oi = 0; oi < arr.size(); ++oi) {
    const auto& jo = arr[oi];
    ObjectAnchors oa;
    oa.name = jo.contains("name") ? jo.at("name").get<std::string>()
                                  : "object_" + std::to_string(oi);
    const std::string tag = "objects[" + std::to_string(oi) + "](" + oa.name + ")";

    // --- time_anchor ---
    if (!jo.contains("time_anchor")) Fail(tag + " に time_anchor がありません");
    oa.time_anchor = ToVector(jo.at("time_anchor"), tag + ".time_anchor");
    if (oa.time_anchor.size() == 0) Fail(tag + ".time_anchor が空です");
    RequireIncreasingPositive(oa.time_anchor, tag + ".time_anchor");

    const Index n_tanchor = oa.time_anchor.size();

    // --- time_freq_anchor（省略可） ---
    if (!jo.contains("time_freq_anchor") || jo.at("time_freq_anchor").is_null()) {
      // 周波数ワープなし。1 行だけ確保してゼロのままにしておく
      oa.time_freq_anchor = MatrixXd::Zero(1, n_tanchor);
      set.objects.push_back(std::move(oa));
      continue;
    }

    const auto& jf = jo.at("time_freq_anchor");
    if (!jf.is_array()) Fail(tag + ".time_freq_anchor は配列の配列である必要があります");
    if (static_cast<Index>(jf.size()) != n_tanchor)
      Fail(tag + ".time_freq_anchor の行数 (" + std::to_string(jf.size()) +
           ") が time_anchor の長さ (" + std::to_string(n_tanchor) +
           ") と一致しません");

    // 各行を読んで最大本数を調べる。行ごとに本数が違ってよい
    std::vector<VectorXd> rows(static_cast<std::size_t>(n_tanchor));
    Index max_n = 1;
    for (Index jj = 0; jj < n_tanchor; ++jj) {
      const std::string rtag =
          tag + ".time_freq_anchor[" + std::to_string(jj) + "]";
      rows[jj] = ToVector(jf[static_cast<std::size_t>(jj)], rtag);
      // 正であることは log を取るため必須。単調性は必須ではない
      // （interp1 の y 側に入るだけなので、逆転しても計算は成立する）
      detail_io::RequirePositive(rows[jj], rtag);
      std::string msg;
      if (!detail_io::CheckIncreasing(rows[jj], rtag, &msg))
        set.warnings.push_back(msg);
      max_n = std::max(max_n, rows[jj].size());
    }

    // 短い行は 0 で詰めたうえで転置して格納する
    oa.time_freq_anchor = MatrixXd::Zero(max_n, n_tanchor);
    for (Index jj = 0; jj < n_tanchor; ++jj)
      oa.time_freq_anchor.col(jj).head(rows[jj].size()) = rows[jj];

    set.objects.push_back(std::move(oa));
  }

  // --- 全素材でアンカー本数が揃っているかを検証 ---
  // モーフィング本体が前提にしている条件なので、ここで早めに弾く
  const Index n_tanchor = set.objects[0].time_anchor.size();
  std::vector<Index> n_freq(static_cast<std::size_t>(n_tanchor));
  for (Index jj = 0; jj < n_tanchor; ++jj)
    n_freq[jj] = (set.objects[0].time_freq_anchor.col(jj).array() != 0.0).count();

  for (std::size_t oi = 1; oi < set.objects.size(); ++oi) {
    const auto& o = set.objects[oi];
    if (o.time_anchor.size() != n_tanchor)
      Fail("素材 '" + o.name + "' の時間アンカー数 (" +
           std::to_string(o.time_anchor.size()) + ") が素材 '" +
           set.objects[0].name + "' (" + std::to_string(n_tanchor) +
           ") と一致しません");
    for (Index jj = 0; jj < n_tanchor; ++jj) {
      const Index c = (o.time_freq_anchor.col(jj).array() != 0.0).count();
      if (c != n_freq[jj])
        Fail("素材 '" + o.name + "' の時刻 " + std::to_string(jj) +
             " における周波数アンカー本数 (" + std::to_string(c) + ") が素材 '" +
             set.objects[0].name + "' (" + std::to_string(n_freq[jj]) +
             ") と一致しません");
    }
  }

  return set;
}

/// JSON ファイルから AnchorSet を読み込む。
/// print_warnings が true なら警告を stderr にも出す（既定）
inline AnchorSet LoadAnchorSet(const std::string& path,
                               bool print_warnings = true) {
  std::ifstream ifs(path);
  if (!ifs) detail_io::Fail("ファイルを開けません: " + path);

  nlohmann::json root;
  try {
    ifs >> root;
  } catch (const nlohmann::json::parse_error& e) {
    detail_io::Fail("JSON の構文エラー (" + path + "): " + e.what());
  }
  AnchorSet set = ParseAnchorSet(root);

  if (print_warnings)
    for (const auto& w : set.warnings)
      std::cerr << "警告: " << w << std::endl;

  return set;
}

// ===========================================================================
// MorphObject への適用
// ===========================================================================

/// 1 素材にアンカーを適用する。WORLD パラメータに照らして値域も検証する。
/// warnings が null なら警告は stderr に出る
inline void ApplyAnchors(const ObjectAnchors& src, MorphObject* dst,
                         std::vector<std::string>* warnings = nullptr) {
  const double fs = dst->world_parameter.sampling_frequency;
  const double duration = dst->world_parameter.duration();

  auto warn = [&](const std::string& m) {
    if (warnings) warnings->push_back(m);
    else std::cerr << "警告: " << m << std::endl;
  };

  // 発話長を超える時間アンカーは区間長が 0 以下になり log が取れないので誤り
  if (!(src.time_anchor[src.time_anchor.size() - 1] < duration))
    detail_io::Fail("素材 '" + src.name + "' の最終時間アンカー (" +
                    std::to_string(src.time_anchor[src.time_anchor.size() - 1]) +
                    " s) が発話長 (" + std::to_string(duration) + " s) 以上です");

  // Nyquist 以上のアンカーは最終区間の対数区間長を負にするが、
  // 素材間の重み付き和で打ち消されることがあるので警告にとどめる
  const double nyquist = fs / 2.0;
  for (Index jj = 0; jj < src.time_freq_anchor.cols(); ++jj)
    for (Index kk = 0; kk < src.time_freq_anchor.rows(); ++kk) {
      const double f = src.time_freq_anchor(kk, jj);
      if (f != 0.0 && !(f < nyquist)) {
        warn("素材 '" + src.name + "' の時間アンカー " + std::to_string(jj) +
             " に Nyquist (" + std::to_string(nyquist) + " Hz) 以上の周波数 " +
             std::to_string(f) + " Hz があります");
        jj = src.time_freq_anchor.cols();  // 1 回だけ報告して抜ける
        break;
      }
    }

  dst->time_anchor = src.time_anchor;
  dst->time_freq_anchor = src.time_freq_anchor;
}

/// AnchorSet を素材リストへ順番に適用する
inline void ApplyAnchorSet(const AnchorSet& set, std::vector<MorphObject>* objs,
                           std::vector<std::string>* warnings = nullptr) {
  if (set.size() != objs->size())
    detail_io::Fail("JSON の素材数 (" + std::to_string(set.size()) +
                    ") と MorphObject の数 (" + std::to_string(objs->size()) +
                    ") が一致しません");
  for (std::size_t i = 0; i < objs->size(); ++i)
    ApplyAnchors(set.objects[i], &(*objs)[i], warnings);
}

/// 名前で対応づけて適用する。WORLD 分析の順序と JSON の並びが違う場合に使う
inline void ApplyAnchorSet(const AnchorSet& set,
                           const std::vector<std::string>& names,
                           std::vector<MorphObject>* objs,
                           std::vector<std::string>* warnings = nullptr) {
  if (names.size() != objs->size())
    detail_io::Fail("names の数と MorphObject の数が一致しません");
  for (std::size_t i = 0; i < objs->size(); ++i)
    ApplyAnchors(set.at(names[i]), &(*objs)[i], warnings);
}

// ===========================================================================
// 書き出し（GUI でアンカーを編集したあとの保存用）
// ===========================================================================

/// MorphObject のアンカーを JSON に変換する。
/// 0 詰めされた要素は落として、行ごとの本数をそのまま書き出す
inline nlohmann::json ToJson(const std::vector<MorphObject>& objs,
                             const std::vector<std::string>& names = {}) {
  nlohmann::json root;
  root["version"] = 1;
  root["objects"] = nlohmann::json::array();

  for (std::size_t oi = 0; oi < objs.size(); ++oi) {
    nlohmann::json jo;
    jo["name"] = (oi < names.size()) ? names[oi] : "object_" + std::to_string(oi);

    const VectorXd& ta = objs[oi].time_anchor;
    jo["time_anchor"] = std::vector<double>(ta.data(), ta.data() + ta.size());

    const MatrixXd& tf = objs[oi].time_freq_anchor;
    nlohmann::json rows = nlohmann::json::array();
    for (Index jj = 0; jj < tf.cols(); ++jj) {
      std::vector<double> row;
      for (Index kk = 0; kk < tf.rows(); ++kk)
        if (tf(kk, jj) != 0.0) row.push_back(tf(kk, jj));
      rows.push_back(row);
    }
    jo["time_freq_anchor"] = rows;
    root["objects"].push_back(jo);
  }
  return root;
}

/// JSON ファイルへ書き出す
inline void SaveAnchorSet(const std::vector<MorphObject>& objs,
                          const std::string& path,
                          const std::vector<std::string>& names = {}) {
  std::ofstream ofs(path);
  if (!ofs) detail_io::Fail("ファイルを書き込めません: " + path);
  ofs << std::setw(2) << ToJson(objs, names) << std::endl;
}

}  // namespace io
}  // namespace tcmorph

#endif  // TCMORPH_ANCHOR_IO_HPP_
