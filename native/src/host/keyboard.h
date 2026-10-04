// PC keyboard emulation: SDL keys to set-1 scan codes and BIOS key codes.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>

namespace f19 {

class Machine;

// True if `sc` has a PC equivalent the game can receive.
bool key_supported(SDL_Scancode sc);

// Deliver a key going down or up, with `mod` the modifiers held: the raw
// make/break code goes through port 60h + IRQ1 (the game's own keyboard
// handler sees held keys) along with the code the BIOS buffers for menus
// and typed commands. The caller holds the machine's lock.
void send_key(Machine& m, SDL_Scancode sc, SDL_Keymod mod, bool down);

}  // namespace f19
