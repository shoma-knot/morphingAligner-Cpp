#include "miniaudio_cpp/audio.hpp"

// miniaudio.h is included HERE and ONLY here (in C++ context).
// The public header (audio.hpp) never touches it.
#include "miniaudio.h"

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

} // anonymous namespace

// ── Engine impl ─────────────────────────────────────────────

struct engine_impl {
    ma_engine handle{};

    // In-memory PCM playback (one at a time). Owns the copied audio buffer.
    ma_sound         pcm_sound{};
    ma_audio_buffer* pcm_buffer{nullptr};
    bool             pcm_active{false};

    engine_impl() {
        check(ma_engine_init(nullptr, &handle), "engine init");
    }
    ~engine_impl() {
        stop_pcm();
        ma_engine_uninit(&handle);
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
    check(ma_engine_play_sound(&impl_->handle,
                               std::string{path}.c_str(), nullptr),
          "play_oneshot");
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
    check(ma_sound_init_from_file(&eng.impl_->handle,
                                  std::string{path}.c_str(),
                                  flags, nullptr, nullptr,
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
    check(ma_decoder_init_file(std::string{path}.c_str(),
                               &cfg, &dec),
          "decoder init");

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
    check(ma_encoder_init_file(std::string{path}.c_str(), &cfg, &enc),
          "encoder init");
    ma_uint64 written = 0;
    ma_result r =
        ma_encoder_write_pcm_frames(&enc, frames, frame_count, &written);
    ma_encoder_uninit(&enc);
    check(r, "encoder write");
}

} // namespace ma
