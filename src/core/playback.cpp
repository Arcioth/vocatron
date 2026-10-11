#include "playback.hpp"

#include "miniaudio_prefix.hpp"

#include "miniaudio.h"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>

namespace vocatron {
namespace {

void on_audio(ma_device *device, void *output, const void * /*input*/, ma_uint32 frame_count) {
  auto *dst = static_cast<float *>(output);
  if (dst == nullptr) {
    return;
  }
  auto *user = static_cast<PlaybackUser *>(device->pUserData);
  if (user == nullptr || user->ring == nullptr) {
    std::memset(dst, 0, static_cast<std::size_t>(frame_count) * sizeof(float));
    return;
  }
  const std::size_t n = user->ring->try_read(dst, frame_count);
  if (n > 0 && user->played != nullptr) {
    user->played->fetch_add(n, std::memory_order_relaxed);
  }
  if (n < frame_count) {
    std::memset(dst + n, 0, (static_cast<std::size_t>(frame_count) - n) * sizeof(float));
    if (user->underruns != nullptr) {
      user->underruns->fetch_add(1, std::memory_order_relaxed);
    }
  }
}

ma_backend backend_from_name(const char *which) {
  if (which != nullptr && std::strcmp(which, "null") == 0) {
    return ma_backend_null;
  }
  return ma_backend_pulseaudio;
}

// miniaudio loads libpulse by name. The nix dev-shell loader does not
// search the system profile, so the name alone fails. Opening the real
// file first lets the later lookup find it already loaded.
void preload_pulse() {
  const char *paths[] = {
      "/run/current-system/sw/lib/libpulse.so.0",
      "/usr/lib/libpulse.so.0",
  };
  for (const char *path : paths) {
    if (dlopen(path, RTLD_NOW | RTLD_GLOBAL) != nullptr) {
      return;
    }
  }
}

} // namespace

struct Speakers::Impl {
  ma_context context{};
  ma_device device{};
  bool context_ready = false;
  bool device_ready = false;
  bool started = false;
  const char *backend = "";
};

Speakers::Speakers() = default;

Speakers::~Speakers() {
  if (impl_ == nullptr) {
    return;
  }
  if (impl_->started) {
    ma_device_stop(&impl_->device);
    impl_->started = false;
  }
  if (impl_->device_ready) {
    ma_device_uninit(&impl_->device);
    impl_->device_ready = false;
  }
  if (impl_->context_ready) {
    ma_context_uninit(&impl_->context);
    impl_->context_ready = false;
  }
}

bool Speakers::open(PlaybackUser *user, const char *which, const char **backend_name) {
  impl_ = std::make_unique<Impl>();
  const ma_backend backend = backend_from_name(which);
  if (backend == ma_backend_pulseaudio) {
    preload_pulse();
  }

  ma_context_config config = ma_context_config_init();
  config.pulse.pApplicationName = "vocatron";
  const ma_result context_rc = ma_context_init(&backend, 1, &config, &impl_->context);
  if (context_rc != MA_SUCCESS) {
    std::fprintf(stderr, "could not open the audio backend (%s)\n",
                 ma_result_description(context_rc));
    if (backend == ma_backend_pulseaudio) {
      std::fprintf(stderr, "PipeWire's PulseAudio socket did not answer, or libpulse.so.0 "
                           "could not be loaded.\n");
    }
    return false;
  }
  impl_->context_ready = true;
  impl_->backend = ma_get_backend_name(impl_->context.backend);
  if (backend_name != nullptr) {
    *backend_name = impl_->backend;
  }

  ma_device_config device_config = ma_device_config_init(ma_device_type_playback);
  device_config.playback.format = ma_format_f32;
  device_config.playback.channels = 1;
  device_config.sampleRate = 48000;
  device_config.dataCallback = on_audio;
  device_config.pUserData = user;
  const ma_result device_rc = ma_device_init(&impl_->context, &device_config, &impl_->device);
  if (device_rc != MA_SUCCESS) {
    std::fprintf(stderr, "could not open the playback device (%s)\n",
                 ma_result_description(device_rc));
    return false;
  }
  impl_->device_ready = true;
  return true;
}

bool Speakers::start() {
  if (impl_ == nullptr || !impl_->device_ready || impl_->started) {
    return impl_ != nullptr && impl_->started;
  }
  const ma_result rc = ma_device_start(&impl_->device);
  if (rc != MA_SUCCESS) {
    std::fprintf(stderr, "could not start playback (%s)\n", ma_result_description(rc));
    return false;
  }
  impl_->started = true;
  return true;
}

void Speakers::stop() {
  if (impl_ != nullptr && impl_->started) {
    ma_device_stop(&impl_->device);
    impl_->started = false;
  }
}

} // namespace vocatron
