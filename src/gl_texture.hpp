#pragma once

/// @file gl_texture.hpp
/// @brief OpenGL テクスチャの所有者（RAII）。
///
/// 破棄・差し替え時に glDeleteTextures を呼ぶ。コピーは不可、ムーブのみ。
/// GL を触るので、作成・破棄は GL コンテキストがあるメインスレッドで行うこと
/// （App は main.cpp でウィンドウを閉じる前に破棄されるので、メンバに持ってよい）。

#include <utility>

#include <imgui.h>    // ImTextureID

class GlTexture {
public:
    GlTexture() = default;
    explicit GlTexture(unsigned int id) : id_(id) {}
    ~GlTexture() { reset(); }

    GlTexture(GlTexture&& o) noexcept : id_(std::exchange(o.id_, 0u)) {}
    GlTexture& operator=(GlTexture&& o) noexcept {
        if (this != &o) {
            reset();
            id_ = std::exchange(o.id_, 0u);
        }
        return *this;
    }
    GlTexture(const GlTexture&)            = delete;
    GlTexture& operator=(const GlTexture&) = delete;

    // テクスチャを解放して空にする。
    void reset();

    unsigned int id() const { return id_; }
    ImTextureID  imgui_id() const { return static_cast<ImTextureID>(id_); }
    explicit     operator bool() const { return id_ != 0; }

private:
    unsigned int id_ = 0;    // 0 = なし
};
