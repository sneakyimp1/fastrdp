#pragma once

#include <cstdint>

#include <SDL3/SDL_scancode.h>

namespace fastrdp {

// Maps an SDL (USB HID based) scancode to an RDP scancode as encoded by
// <freerdp/scancode.h> (code | extended flag). Returns 0 for unmapped keys.
uint32_t sdlToRdpScancode(SDL_Scancode sc);

} // namespace fastrdp
