#pragma once

/// @file auto_anchors.hpp
/// @brief 音素アライメントとフォルマントからのアンカー自動生成。

#include <string>
#include <vector>

struct Anchor;
struct Track;

// アンカー自動生成の結果。error が空なら成功（anchors を App::anchors と置き換える）。
struct AutoAnchorResult {
    std::vector<Anchor>      anchors;
    std::vector<std::string> warnings;    // 生成はしたが注意が要る点（ログに出す）
    std::string              error;
    int                      n_freq = 0;    // 打った周波数アンカーの総数（ログ用）

    bool ok() const { return error.empty(); }
};

// base/target の音素アライメント（Track::segmentation）とフォルマントの移動平均
// （Track::formants_ma）から、時間アンカーと周波数アンカーを作る。
//   1. 無音を除いた音素を base/target で先頭から順に対応させ、各音素の始まりと最後の
//      音素の終わり（途中のポーズの前の音素の終わりも、両側にポーズがあれば）に時間アンカー。
//   2. 各音素の区間を divisions 等分する位置にも時間アンカー（divisions=1 なら追加なし）。
//   3. 各時間アンカーの時刻で移動平均フォルマントの値を読み、base/target の両方で値が
//      取れた F_k を周波数アンカーにする。
// 無音を除いた音素の数が base/target で違う場合は対応がとれないので失敗にする。
AutoAnchorResult generate_auto_anchors(const Track& base, const Track& target, int divisions);
