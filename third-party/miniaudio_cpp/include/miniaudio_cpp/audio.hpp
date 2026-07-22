#pragma once

/// @file audio.hpp
/// @brief Lightweight C++ wrapper for miniaudio.
///
/// This header intentionally does NOT include miniaudio.h.
/// All platform-specific types are hidden behind pimpl so that
/// the C API never leaks into your translation units.

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ma {

// ── Exceptions ──────────────────────────────────────────────

/// Thrown on any miniaudio failure.
class error : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// ── Forward declarations (pimpl) ────────────────────────────

struct engine_impl;
struct sound_impl;
struct decoder_impl;

// ── Engine ──────────────────────────────────────────────────

/// Owns the audio device and mixes all sounds.
/// Typically one instance per application.
class engine {
  public:
    engine();
    ~engine();

    engine(engine&&) noexcept;
    engine& operator=(engine&&) noexcept;
    engine(const engine&) = delete;
    engine& operator=(const engine&) = delete;

    /// Fire-and-forget playback of a file.
    void play_oneshot(std::string_view path);

    /// Fire-and-forget playback of an in-memory interleaved float PCM buffer.
    /// The data is copied, so `frames` need not outlive the call. Only one such
    /// buffer plays at a time; a new call replaces the previous one.
    void play_pcm(const float* frames, std::uint64_t frame_count,
                  std::uint32_t channels, std::uint32_t sample_rate);

    /// Sample rate of the underlying device.
    std::uint32_t sample_rate() const;

    /// Number of channels of the underlying device.
    std::uint32_t channels() const;

  private:
    friend class sound;
    std::unique_ptr<engine_impl> impl_;
};

// ── Sound ───────────────────────────────────────────────────

/// Represents a single playable audio source.
/// Supports one-shot effects (preloaded) and streaming (for BGM).
class sound {
  public:
    enum class type { decode, stream };

    sound(engine& eng, std::string_view path,
          type t = type::decode);
    ~sound();

    sound(sound&&) noexcept;
    sound& operator=(sound&&) noexcept;
    sound(const sound&) = delete;
    sound& operator=(const sound&) = delete;

    void play();
    void stop();
    void pause();

    /// Restart from the beginning (even if already playing).
    void replay();

    bool is_playing() const;
    bool is_looping() const;
    void set_looping(bool loop);

    float volume() const;
    void  set_volume(float v);

    float pitch() const;
    void  set_pitch(float p);

    float pan() const;
    void  set_pan(float p);

    /// Seek to a specific PCM frame.
    void seek(std::uint64_t frame);

    /// Current playback position in PCM frames.
    std::uint64_t cursor() const;

    /// Total length in PCM frames (0 for streams).
    std::uint64_t length() const;

  private:
    std::unique_ptr<sound_impl> impl_;
};

// ── Decoder (offline) ───────────────────────────────────────

/// Decodes an entire audio file into a PCM float buffer.
/// Useful when you need raw sample data (e.g. for visualization).
class decoder {
  public:
    explicit decoder(std::string_view path);
    ~decoder();

    decoder(decoder&&) noexcept;
    decoder& operator=(decoder&&) noexcept;
    decoder(const decoder&) = delete;
    decoder& operator=(const decoder&) = delete;

    /// Pointer to interleaved float samples.
    const float*  data()        const;
    std::uint64_t frame_count() const;
    std::uint32_t channels()    const;
    std::uint32_t sample_rate() const;

  private:
    std::unique_ptr<decoder_impl> impl_;
};

// ── WAV writer (offline) ────────────────────────────────────

/// Write interleaved float PCM to a WAV file (32-bit float format).
/// Throws ma::error on failure.
void write_wav(std::string_view path, const float* frames,
               std::uint64_t frame_count, std::uint32_t channels,
               std::uint32_t sample_rate);

} // namespace ma
