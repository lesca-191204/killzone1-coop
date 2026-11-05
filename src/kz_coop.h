#pragma once
// Fork (KZ1 co-op): player 2 in the campaign, Lua access to the level script, per-PC camera (kz_coop.cpp).

#include <cstdint>
#include <string>

class PS2Runtime;
struct R5900Context;

void kzCoopInstall(PS2Runtime &runtime);
// Queue Lua source to run in the level script's state at the next frame limiter pass (same frame on every PC).
void kzLuaQueue(const std::string &code);
// From the lockstep limiter wrapper. Called before the game's frame runs:
//  kzCoopResume: true if this pass is the return from a Lua call started by kzCoopFrame (the wrapper then only runs the
//                original limiter, the frame was already counted).
//  kzCoopFrame:  per-frame co-op work; true if it redirected the guest into a Lua call (the wrapper returns at once).
bool kzCoopResume(uint8_t *rdram, R5900Context *ctx);
bool kzCoopFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t frame);
// Player 2's entity (0 if none) and the local player's entity (the camera follows it).
uint32_t kzCoopPlayer2();
