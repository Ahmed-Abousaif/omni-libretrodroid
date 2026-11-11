#pragma once

#include <stdint.h>

// Omnidroid private libretro extension. Keep this ABI identical in the core.
// UTF-16 lengths exclude the terminator. Pointers are valid only for the call.
#define OMNI_ENVIRONMENT_GET_TEXT_INPUT_INTERFACE (0x20000 | 0x4f01)
#define OMNI_TEXT_INPUT_VERSION 1
#define OMNI_TEXT_INPUT_CAPACITY 4096

struct omni_text_input_interface
{
    uint32_t version;
    void (*show)(uint64_t session, const uint16_t* text, uint32_t length, uint32_t max_length);
    void (*hide)(uint64_t session);
    bool (*poll)(uint64_t session, uint16_t* text, uint32_t capacity, uint32_t* length, bool* confirmed);
};
