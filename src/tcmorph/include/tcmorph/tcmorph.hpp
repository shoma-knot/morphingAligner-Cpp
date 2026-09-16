// tcmorph/tcmorph.hpp
//
// tcmorph の全ヘッダをまとめて取り込む便宜ヘッダ。
// 必要なものだけを個別に include しても構わない。
//
//   generalized_tc_morphing.hpp  generalizedTCmorphing.m の移植（N 素材）
//   word_tv2w_morphing.hpp       wordTV2WmorphingEngineRev.m の移植（2 素材）
//   anchor_io.hpp                アンカーの JSON 読み書き
//   world_io.hpp                 WORLD パラメータの npy 読み書き
//
// 依存:
//   Eigen 3.4          すべてのヘッダ
//   nlohmann/json      anchor_io.hpp / world_io.hpp のみ
//   OpenMP             任意（あれば generalized_tc_morphing.hpp が並列化する）
//
// I/O が不要なら generalized_tc_morphing.hpp か word_tv2w_morphing.hpp だけを
// include すればよく、その場合 nlohmann/json への依存は発生しない。

#ifndef TCMORPH_TCMORPH_HPP_
#define TCMORPH_TCMORPH_HPP_

#include <tcmorph/generalized_tc_morphing.hpp>
#include <tcmorph/word_tv2w_morphing.hpp>
#include <tcmorph/anchor_io.hpp>
#include <tcmorph/world_io.hpp>

/// ライブラリのバージョン
#define TCMORPH_VERSION_MAJOR 1
#define TCMORPH_VERSION_MINOR 0
#define TCMORPH_VERSION_PATCH 0
#define TCMORPH_VERSION_STRING "1.0.0"

#endif  // TCMORPH_TCMORPH_HPP_
