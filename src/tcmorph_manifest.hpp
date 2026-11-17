#pragma once

/// @file tcmorph_manifest.hpp
/// @brief tcmorph の mat2json.py が anchors.json と一緒に書き出す manifest.json を読む。
///
/// anchors.json はアンカーしか持たないが、同じフォルダの manifest.json には各素材の音声の場所
/// （objects[i].fileFullPath と fileName）と標本化周波数が書いてある。batch で tcmorph のアンカー
/// JSON を --base / --target なしで渡したとき、ここから音声を決める。WORLD パラメータ（npy）は読まない
/// （音声はこのアプリで解析し直す）。ファイル操作だけで、音声のデコードや App には依存しない。

#include <string>

#include "result.hpp"

// manifest.json から読んだもの。素材は objects[0] を base、objects[1] を target とする（anchors.json と同じ）。
struct TcmorphManifest {
    std::string path;                         // manifest.json のパス
    std::string base_path, target_path;       // 音声のパス（実在を確かめたもの）
    double      base_fs = 0, target_fs = 0;    // manifest に書かれた標本化周波数 [Hz]
    // セッションや書き出しの JSON に "tcmorph" として残す記録（JSON のオブジェクトの文字列）。
    // manifest の objects 以外の項目（source・layout・suggested_alpha・vtl_ratio など）と、
    // anchors.json / manifest.json のパスを持つ。
    std::string record;
};

// anchors_path（tcmorph のアンカー JSON）と同じフォルダの manifest.json を読む。音声のパスは
// fileFullPath と fileName をつないだもので、相対パスなら manifest.json のフォルダが基準。
// manifest.json が無い・形式が不正・音声が見つからない場合は error に理由。
Result<TcmorphManifest> read_tcmorph_manifest(const std::string& anchors_path);

// 音声の実際の標本化周波数（base_fs_actual / target_fs_actual）が manifest と合っているか。
// 合わなければ、別の音声を指している（取り違えや書き出し直し）おそれがあるので error にする。
Status check_manifest_sample_rates(const TcmorphManifest& m, int base_fs_actual, int target_fs_actual);
