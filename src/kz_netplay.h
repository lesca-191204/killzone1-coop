#pragma once
// Fork (KZ1 co-op): input lockstep between two PCs (kz_netplay.cpp).
//
// Both PCs run the whole game deterministically (kz_lockstep + PS2X_DETERMINISTIC); only pad input travels. Every game
// frame (pass of the frame limiter) the input of both players for that frame is fixed ("latched"): the local one is
// sampled and sent ahead (input delay), the remote one is waited for. The game and the co-op code only ever see the
// latched inputs, so both PCs simulate exactly the same frames.
//
// Player 0 = the host (the game's own player, Templar); player 1 = the joiner (co-op partner, kz_coop).

#include <cstddef>
#include <cstdint>
#include <cstring>

class PS2Runtime;

// Raw DualShock reply bytes 2..7 (buttons active-low, then RX RY LX LY), as kzPadProvider fills them.
struct KzNetInput
{
    uint8_t b[6] = {0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80};
    // Mouse aim (kz_aim): motion in 1/8 counts (sensitivity applied) and flags (bit 0: the player uses keyboard/mouse).
    // Both PCs use these exact quantized values, so the aim patch changes the simulation the same way on each.
    int16_t mdx = 0, mdy = 0;
    uint8_t flags = 0;
    static constexpr size_t kWireSize = 11;
    void write(uint8_t *p) const
    {
        std::memcpy(p, b, 6);
        p[6] = static_cast<uint8_t>(mdx);
        p[7] = static_cast<uint8_t>(static_cast<uint16_t>(mdx) >> 8);
        p[8] = static_cast<uint8_t>(mdy);
        p[9] = static_cast<uint8_t>(static_cast<uint16_t>(mdy) >> 8);
        p[10] = flags;
    }
    void read(const uint8_t *p)
    {
        std::memcpy(b, p, 6);
        mdx = static_cast<int16_t>(p[6] | (p[7] << 8));
        mdy = static_cast<int16_t>(p[8] | (p[9] << 8));
        flags = p[10];
    }
    float mouseDx() const { return mdx / 8.0f; }
    float mouseDy() const { return mdy / 8.0f; }
    bool kbm() const { return (flags & 1u) != 0; }
    uint16_t buttons() const { return static_cast<uint16_t>(~(b[0] | (b[1] << 8))); } // KzPadButton mask, active-high
    float axis(int i) const { return (static_cast<int>(b[2 + i]) - 128) / 127.0f; }    // 0 rx, 1 ry, 2 lx, 3 ly
};

void kzNetInstall(PS2Runtime &runtime);
// From the lockstep limiter wrapper, once per game frame, before the game runs it (guest thread).
void kzNetFrame(uint8_t *rdram, uint32_t frame);
bool kzNetActive();        // inputs are latched per frame (netplay session or KZ_NET_LATCH test)
bool kzNetHasPlayer2();    // a second player takes part (connected peer, or KZ_NET_P2SCRIPT test)
int kzNetLocalPlayer();    // 0 on the host / solo, 1 on the joiner
bool kzNetPeerLeft();
uint32_t kzNetPlayer2Char(); // player 2's chosen character: 0 Templar, 1 Luger, 2 Rico (Choyan), 3 Hakha
bool kzNetSessionMode();     // KZ_NET=host|join (or a latch test with scripted player 2), regardless of connection      // a session ran during this level and the other player has left (BYE or timeout)
const KzNetInput &kzNetInputOf(int player); // latched input of that player for the current frame
// Pad provider: port 0 = player 0's latched input while a session runs, else the local pad (kzPadProvider).
bool kzNetPadProvider(int port, int slot, uint8_t *data, size_t size);
// Per-frame state digest hook (kz_lockstep computes it; compared with the peer every KZ_NET_DIGEST frames).
void kzNetSetDigest(uint32_t frame, uint64_t digest);
uint32_t kzNetDigestEvery();
void kzNetSetClock(double (*clock)()); // vblank clock for KZ_AUTOSTART (kz_lockstep)
// KZ_FRAME_PACE=<hz> (use with PS2X_NO_PACING=1): wall-clock pacing once per game frame, from the lockstep limiter
// wrapper. The deterministic scheduler's own pacing sleeps on host deadlines that no longer match the cycle clock (a
// paced session ran at ~6 game fps); sleeping here never changes the simulation.
void kzFramePace();
// Guest vblank rate of the session (the host's frame-rate choice; 60 outside a session). Known after kzNetInstall.
int kzNetSessionVblankRate();
