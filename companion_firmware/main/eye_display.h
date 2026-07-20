#pragma once

#include <cstdint>

#include "esp_err.h"

namespace companion {

// Both physical LCD modules are wired as one serial chain, so they always show
// the same frame.  Use a symmetric neutral-eye asset for every state.
enum class EyeState : std::uint8_t {
    kIdle = 0,
    kListening,
    kThinking,
    kSpeaking,
    kDictation,
    kError,
};

// Initializes the shared 160x160 SPI3 display chain and starts the low-priority
// animation task. Calling this more than once is safe.
esp_err_t eye_display_init();

// Thread-safe; the animation task applies the new state on its next frame.
void eye_display_set_state(EyeState state);

EyeState eye_display_state();

}  // namespace companion
