#include "miniaudio_cpp/audio.hpp"

// miniaudio.h is included HERE and ONLY here (in C++ context).
// The public header (audio.hpp) never touches it.
#include "miniaudio.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ma {

// ── helpers ─────────────────────────────────────────────────

namespace {

void check(ma_result result, const char* context) {
    if (result != MA_SUCCESS) {
        throw error{std::string{context} + ": " +
                    ma_result_description(result)};
    }
}

// ── ファイルのパス ──────────────────────────────────────────
// このラッパーはパスを UTF-8 で受け取る（アプリはファイルダイアログもコマンドライン引数も
// UTF-8 で扱う）。Windows の miniaudio は char* のパスを fopen_s（ANSI のコードページ。日本語の
// Windows なら Shift_JIS）で開くため、ASCII 以外を含むと別の名前のファイルとして扱われて
// 開けない（書き出しでは化けた名前のファイルができる）。Windows ではワイド文字版の API を使う。

#ifdef _WIN32
std::wstring to_wide(std::string_view utf8) {
    return std::filesystem::u8path(std::string{utf8}).wstring();
}
#endif

ma_result decoder_init_file(std::string_view path, const ma_decoder_config* cfg,
                            ma_decoder* dec) {
#ifdef _WIN32
    return ma_decoder_init_file_w(to_wide(path).c_str(), cfg, dec);
#else
    return ma_decoder_init_file(std::string{path}.c_str(), cfg, dec);
#endif
}

ma_result encoder_init_file(std::string_view path, const ma_encoder_config* cfg,
                            ma_encoder* enc) {
#ifdef _WIN32
    return ma_encoder_init_file_w(to_wide(path).c_str(), cfg, enc);
#else
    return ma_encoder_init_file(std::string{path}.c_str(), cfg, enc);
#endif
}

ma_result sound_init_from_file(ma_engine* eng, std::string_view path,
                               ma_uint32 flags, ma_sound* snd) {
#ifdef _WIN32
    return ma_sound_init_from_file_w(eng, to_wide(path).c_str(), flags,
                                     nullptr, nullptr, snd);
#else
    return ma_sound_init_from_file(eng, std::string{path}.c_str(), flags,
                                   nullptr, nullptr, snd);
#endif
}

} // anonymous namespace

// ── Engine impl ─────────────────────────────────────────────

struct engine_impl {
    ma_engine handle{};

    // In-memory PCM playback (one at a time). Owns the copied audio buffer.
    ma_sound         pcm_sound{};
    ma_audio_buffer* pcm_buffer{nullptr};
    bool             pcm_active{false};

    // play_oneshot で鳴らしているファイルの音。ma_engine_play_sound にはワイド文字版が
    // 無いので自前で持ち、鳴り終わったものは次の play_oneshot で片付ける。ma_sound は
    // 中を指すポインタを miniaudio が持つので、動かさないよう unique_ptr で持つ。
    std::vector<std::unique_ptr<ma_sound>> oneshots;

    engine_impl() {
        check(ma_engine_init(nullptr, &handle), "engine init");
    }
    ~engine_impl() {
        stop_pcm();
        for (auto& s : oneshots) ma_sound_uninit(s.get());
        ma_engine_uninit(&handle);
    }

    // 鳴り終わった play_oneshot の音を解放する。
    void reap_oneshots() {
        for (auto it = oneshots.begin(); it != oneshots.end();) {
            if (ma_sound_at_end(it->get())) {
                ma_sound_uninit(it->get());
                it = oneshots.erase(it);
            } else {
                ++it;
            }
        }
    }

    void stop_pcm() {
        if (pcm_active) {
            ma_sound_uninit(&pcm_sound);
            pcm_active = false;
        }
        if (pcm_buffer) {
            ma_audio_buffer_uninit_and_free(pcm_buffer);
            pcm_buffer = nullptr;
        }
    }
};

engine::engine() : impl_{std::make_unique<engine_impl>()} {}
engine::~engine() = default;
engine::engine(engine&&) noexcept = default;
engine& engine::operator=(engine&&) noexcept = default;

void engine::play_oneshot(std::string_view path) {
    impl_->reap_oneshots();
    auto snd = std::make_unique<ma_sound>();
    check(sound_init_from_file(&impl_->handle, path, 0, snd.get()),
          "play_oneshot");
    const ma_result r = ma_sound_start(snd.get());
    if (r != MA_SUCCESS) {
        ma_sound_uninit(snd.get());
        check(r, "play_oneshot");
    }
    impl_->oneshots.push_back(std::move(snd));
}

void engine::play_pcm(const float* frames, std::uint64_t frame_count,
                      std::uint32_t channels, std::uint32_t sample_rate) {
    impl_->stop_pcm();    // replace any previous in-memory playback

    ma_audio_buffer_config cfg = ma_audio_buffer_config_init(
        ma_format_f32, channels, frame_count, frames, nullptr);
    cfg.sampleRate = sample_rate;
    check(ma_audio_buffer_alloc_and_init(&cfg, &impl_->pcm_buffer),
          "audio buffer init");    // copies the data

    check(ma_sound_init_from_data_source(
              &impl_->handle,
              reinterpret_cast<ma_data_source*>(impl_->pcm_buffer),
              0, nullptr, &impl_->pcm_sound),
          "pcm sound init");
    impl_->pcm_active = true;
    check(ma_sound_start(&impl_->pcm_sound), "pcm sound start");
}

std::uint32_t engine::sample_rate() const {
    return ma_engine_get_sample_rate(&impl_->handle);
}

std::uint32_t engine::channels() const {
    return ma_engine_get_channels(&impl_->handle);
}

// ── Sound impl ──────────────────────────────────────────────

struct sound_impl {
    ma_sound handle{};
    bool     owns{false};

    ~sound_impl() {
        if (owns) ma_sound_uninit(&handle);
    }
};

sound::sound(engine& eng, std::string_view path, type t)
    : impl_{std::make_unique<sound_impl>()} {
    ma_uint32 flags = 0;
    if (t == type::stream) {
        flags |= MA_SOUND_FLAG_STREAM;
    }
    check(sound_init_from_file(&eng.impl_->handle, path, flags,
                               &impl_->handle),
          "sound init");
    impl_->owns = true;
}

sound::~sound() = default;
sound::sound(sound&&) noexcept = default;
sound& sound::operator=(sound&&) noexcept = default;

void sound::play() {
    check(ma_sound_start(&impl_->handle), "sound play");
}

void sound::stop() {
    check(ma_sound_stop(&impl_->handle), "sound stop");
}

void sound::pause() {
    check(ma_sound_stop(&impl_->handle), "sound pause");
}

void sound::replay() {
    ma_sound_stop(&impl_->handle);
    ma_sound_seek_to_pcm_frame(&impl_->handle, 0);
    check(ma_sound_start(&impl_->handle), "sound replay");
}

bool sound::is_playing() const {
    return ma_sound_is_playing(&impl_->handle) != 0;
}

bool sound::is_looping() const {
    return ma_sound_is_looping(&impl_->handle) != 0;
}

void sound::set_looping(bool loop) {
    ma_sound_set_looping(&impl_->handle, loop ? MA_TRUE : MA_FALSE);
}

float sound::volume() const {
    return ma_sound_get_volume(&impl_->handle);
}

void sound::set_volume(float v) {
    ma_sound_set_volume(&impl_->handle, v);
}

float sound::pitch() const {
    return ma_sound_get_pitch(&impl_->handle);
}

void sound::set_pitch(float p) {
    ma_sound_set_pitch(&impl_->handle, p);
}

float sound::pan() const {
    return ma_sound_get_pan(&impl_->handle);
}

void sound::set_pan(float p) {
    ma_sound_set_pan(&impl_->handle, p);
}

void sound::seek(std::uint64_t frame) {
    check(ma_sound_seek_to_pcm_frame(&impl_->handle, frame),
          "sound seek");
}

std::uint64_t sound::cursor() const {
    ma_uint64 c = 0;
    ma_sound_get_cursor_in_pcm_frames(&impl_->handle, &c);
    return c;
}

std::uint64_t sound::length() const {
    ma_uint64 len = 0;
    ma_sound_get_length_in_pcm_frames(&impl_->handle, &len);
    return len;
}

// ── Decoder impl ────────────────────────────────────────────

struct decoder_impl {
    std::vector<float> samples;
    std::uint64_t      frames{};
    std::uint32_t      ch{};
    std::uint32_t      rate{};
};

decoder::decoder(std::string_view path)
    : impl_{std::make_unique<decoder_impl>()} {
    ma_decoder dec;
    ma_decoder_config cfg =
        ma_decoder_config_init(ma_format_f32, 0, 0);
    check(decoder_init_file(path, &cfg, &dec), "decoder init");

    ma_uint64 total = 0;
    ma_decoder_get_length_in_pcm_frames(&dec, &total);

    impl_->ch   = dec.outputChannels;
    impl_->rate  = dec.outputSampleRate;
    impl_->frames = total;
    impl_->samples.resize(total * impl_->ch);

    ma_uint64 read = 0;
    ma_decoder_read_pcm_frames(&dec, impl_->samples.data(),
                               total, &read);
    impl_->frames = read;
    impl_->samples.resize(read * impl_->ch);

    ma_decoder_uninit(&dec);
}

decoder::~decoder() = default;
decoder::decoder(decoder&&) noexcept = default;
decoder& decoder::operator=(decoder&&) noexcept = default;

const float*  decoder::data()        const { return impl_->samples.data(); }
std::uint64_t decoder::frame_count() const { return impl_->frames; }
std::uint32_t decoder::channels()    const { return impl_->ch; }
std::uint32_t decoder::sample_rate() const { return impl_->rate; }

// ── WAV writer ──────────────────────────────────────────────

void write_wav(std::string_view path, const float* frames,
               std::uint64_t frame_count, std::uint32_t channels,
               std::uint32_t sample_rate) {
    ma_encoder_config cfg = ma_encoder_config_init(
        ma_encoding_format_wav, ma_format_f32, channels, sample_rate);
    ma_encoder enc;
    check(encoder_init_file(path, &cfg, &enc), "encoder init");
    ma_uint64 written = 0;
    ma_result r =
        ma_encoder_write_pcm_frames(&enc, frames, frame_count, &written);
    ma_encoder_uninit(&enc);
    check(r, "encoder write");
}

} // namespace ma
