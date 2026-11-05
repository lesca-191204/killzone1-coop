#pragma once
// Fork (KZ1 co-op): fixed time step and per-frame RAM hashes for determinism tests (kz_lockstep.cpp).

#include <cstdint>

class PS2Runtime;

void kzLockstepInstall(PS2Runtime &runtime); // from applyKillzone, after the other hooks on the frame limiter
bool kzLockstepEnabled();
void kzLockstepSetStep(uint32_t step); // vblanks per game frame (1 or 2); before the game boots
uint32_t kzLockstepStep();
uint32_t kzLockstepFrame(); // game frames since boot (passes of the frame limiter)
