// Fork (KZ1 co-op): determinism tools for input lockstep.
//
// KZ_LOCKSTEP=1      Fixed time step: every pass of the game's frame limiter (kzAddrs().frameLimiter, US FUN_001bff10)
//                    advances the game by exactly one tick, whatever the host's speed. The original reads how many vblanks
//                    went by since the last frame (wall-clock driven in the port) and steps the simulation by that much.
//                    The wrapper sets the limiter's "last vblank" (+0x68) to counter - 1 just before the original runs, so
//                    its delta (elapsed ticks, +0x64) is 1. The guest still waits for vblanks elsewhere (render kick), so
//                    display pacing is unchanged.
// KZ_HASH_LOG=path   One line per game frame at the limiter entry (a fixed point of the main loop):
//                    "<frame> <vsync> <elapsed> <hash> <32 hashes of 1 MB blocks>" over the 32 MB of EE RAM.
// KZ_HASH_EVERY=N    Hash every N frames (default 1). KZ_HASH_FROM=F: start at frame F.
// KZ_HASH_STOP=F     Exit the process after frame F (test runs of a fixed length).
#include "kz_lockstep.h"

#include "kz_addrs.h"
#include "kz_timing.h"
#include "kz_input.h"
#include "kz_coop.h"
#include "kz_netplay.h"
#include "kz_region.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_host_gs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

extern uint32_t (*g_ps2Cop0CountHook)(uint64_t eeCycle);
extern std::FILE *g_ps2SchedTrace; // EeScheduler.cpp (fork) // ext/PS2Recomp EeScheduler.cpp (fork)
extern std::atomic<uint64_t> g_ps2LastGameFrameTick; // EeScheduler.cpp (fork)
extern std::atomic<bool> g_ps2InMovie;               // EeScheduler.cpp (fork): no movie plays during a game frame

namespace
{
    constexpr uint32_t kRamMask = 0x01FFFFFFu;
    constexpr uint32_t kRamSize = 32u * 1024u * 1024u;
    constexpr uint32_t kBlock = 1024u * 1024u;
    constexpr uint32_t kBlocks = kRamSize / kBlock;

    PS2Runtime::RecompiledFunction g_orig = nullptr;
    bool g_lockstep = false;
    // KZ_LOCKSTEP_STEP=N (1 or 2, default 1): vblank ticks per game frame. 2 = the game's own pace on the console (30 fps,
    // elapsed 2 per frame): real-time speed needs half the host frames of step 1 (pace with KZ_FRAME_PACE=30).
    // Mutable: a netplay session sets it from the host's frame-rate choice before the game boots (kzLockstepSetStep).
    uint32_t g_step = []() {
        const char *e = std::getenv("KZ_LOCKSTEP_STEP");
        const int v = e ? std::atoi(e) : 1;
        return static_cast<uint32_t>(v >= 2 ? 2 : 1);
    }();
    std::FILE *g_hashLog = nullptr;
    uint32_t g_hashEvery = 1;
    uint32_t g_hashFrom = 0;
    uint32_t g_stopAt = 0;
    std::atomic<uint32_t> g_frame{0};
    void countStartLevel(uint8_t *rdram);
    std::atomic<uint32_t> g_countChecks{0};

    uint32_t rd32(const uint8_t *ram, uint32_t a)
    {
        uint32_t v;
        std::memcpy(&v, ram + (a & kRamMask), 4);
        return v;
    }
    void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & kRamMask), &v, 4); }

    uint64_t hashBlock(const uint8_t *p, uint32_t n)
    {
        uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
        const uint64_t *w = reinterpret_cast<const uint64_t *>(p);
        for (uint32_t i = 0; i < n / 8; ++i)
        {
            h ^= w[i];
            h *= 0xFF51AFD7ED558CCDull;
            h ^= h >> 29;
        }
        return h;
    }

    void logHash(const uint8_t *rdram, uint32_t frame, uint32_t elapsed)
    {
        uint64_t blocks[kBlocks];
        uint64_t all = 0;
        for (uint32_t b = 0; b < kBlocks; ++b)
        {
            blocks[b] = hashBlock(rdram + b * kBlock, kBlock);
            all = (all ^ blocks[b]) * 0x100000001B3ull;
        }
        std::fprintf(g_hashLog, "%u %u %u %016llx", frame, rd32(rdram, kzAddrs().vsyncCounter), elapsed,
                     static_cast<unsigned long long>(all));
        for (uint32_t b = 0; b < kBlocks; ++b)
            std::fprintf(g_hashLog, " %016llx", static_cast<unsigned long long>(blocks[b]));
        std::fputc('\n', g_hashLog);
    }

    // KZ_STATE_LOG=path: per frame, the game state that matters for lockstep, read from the game's own list of live objects
    // (world object at [worldPtr]: +0x8 count, +0x10 array of 8-byte entries, object pointer at +4; the list the checkpoint
    // scan FUN_001c5330 queries through FUN_0017d318). In list order: each object's vtable; for characters (+0x31C is a
    // character controller) also health (+0x30C), position (+0x50/54/58) and heading (+0x78) as exact float bits.
    // Line: "<frame> <objects> <characters> <hash>"; every KZ_STATE_DUMP frames (default 300) the characters in full.
    std::FILE *g_stateLog = nullptr;
    uint32_t g_stateDump = 300;
    // Global random generator state (Numerical Recipes LCG x*1664525+1013904223; ES FUN_00140580/FUN_001405b0).
    uint32_t rngAddr()
    {
        if (kzIsEs())
            return 0x0055B300u; // es_map_claude.py: 45/45 refs
        if (kzRegion() == KzRegion::Us)
            return 0x0055A680u;
        return 0;
    }
    uint32_t worldPtrAddr()
    {
        if (kzIsEs())
            return 0x0055FD78u; // es_map_claude.py: 352/352 lui/lo refs agree
        if (kzRegion() == KzRegion::Us)
            return 0x0055F0F8u;
        return 0; // PAL SCES-52893: not mapped
    }
    // KZ_STATE_DIAG=1: at frames 300 and 900, list every memory-scan character candidate with the controller offsets
    // that point back to the object (to tell live characters from stale heap copies).
    void diagChars(const uint8_t *ram, uint32_t frame)
    {
        const uint32_t ctlVt = kzAddrs().ctlVtable;
        for (uint32_t a = 0x100000; a + 0x320 < kRamSize; a += 4)
        {
            const uint32_t vt = rd32(ram, a);
            if (vt < 0x500000u || vt >= 0x580000u)
                continue;
            const uint32_t c = rd32(ram, a + 0x31C);
            if (c < 0x100000u || c >= 0x01FFF000u || rd32(ram, c) != ctlVt)
                continue;
            char offs[256] = "";
            size_t n = 0;
            for (uint32_t o = 4; o < 0x400 && n < 200; o += 4)
                if (rd32(ram, c + o) == a)
                    n += std::snprintf(offs + n, sizeof(offs) - n, " +%x", o);
            float x, y;
            const uint32_t wx = rd32(ram, c + 0x50), wy = rd32(ram, c + 0x54);
            std::memcpy(&x, &wx, 4);
            std::memcpy(&y, &wy, 4);
            std::fprintf(g_stateLog, "  D f=%u obj=%06x vt=%06x ctl=%06x pos=%.2f,%.2f back:%s\n", frame, a, vt, c, x, y, offs);
        }
    }

    uint64_t simDigest(const uint8_t *ram);
    void logState(const uint8_t *ram, uint32_t frame)
    {
        static const bool diag = std::getenv("KZ_STATE_DIAG") != nullptr;
        if (diag && (frame == 300 || frame == 900))
            diagChars(ram, frame);
        const uint32_t wp = worldPtrAddr();
        const uint32_t world = wp ? rd32(ram, wp) : 0;
        uint64_t h = 0xCBF29CE484222325ull;
        auto mix = [&h](uint32_t w) { h = (h ^ w) * 0x100000001B3ull; };
        uint32_t n = 0, chars = 0;
        const bool dump = g_stateDump && frame % g_stateDump == 0;
        static std::string dumpText;
        dumpText.clear();
        if (world >= 0x100000u && world < kRamSize)
        {
            n = rd32(ram, world + 0x8);
            const uint32_t arr = rd32(ram, world + 0x10);
            if (n > 20000u || arr < 0x100000u || arr >= kRamSize)
                n = 0;
            const uint32_t ctlVt = kzAddrs().ctlVtable;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t obj = rd32(ram, arr + i * 8 + 4);
                if (obj < 0x100000u || obj + 0x320 >= kRamSize)
                {
                    mix(0xDEAD0000u | i);
                    continue;
                }
                const uint32_t vt = rd32(ram, obj);
                mix(vt);
                const uint32_t c = rd32(ram, obj + 0x31C);
                if (c >= 0x100000u && c < 0x01FFF000u && rd32(ram, c) == ctlVt)
                {
                    ++chars;
                    const uint32_t w[5] = {rd32(ram, obj + 0x30C), rd32(ram, c + 0x50), rd32(ram, c + 0x54), rd32(ram, c + 0x58), rd32(ram, c + 0x78)};
                    for (uint32_t x : w)
                        mix(x);
                    if (dump)
                    {
                        float f[5];
                        std::memcpy(f, w, sizeof(f));
                        char line[200];
                        std::snprintf(line, sizeof(line), "  # [%u] vt=%06x hp=%.3f pos=%.4f,%.4f,%.4f h=%.4f\n", i, vt, f[0], f[1], f[2], f[3], f[4]);
                        dumpText += line;
                    }
                }
            }
        }
        if (dump)
        {
            // the player entity (playerVtable) and its controller
            for (uint32_t a = 0x100000; a + 0x320 < kRamSize; a += 4)
                if (rd32(ram, a) == kzAddrs().playerVtable)
                {
                    const uint32_t c = rd32(ram, a + 0x31C);
                    if (c < 0x100000u || c >= 0x01FFF000u || rd32(ram, c) != kzAddrs().ctlVtable)
                        continue;
                    float f[5];
                    const uint32_t w[5] = {rd32(ram, a + 0x30C), rd32(ram, c + 0x50), rd32(ram, c + 0x54), rd32(ram, c + 0x58), rd32(ram, c + 0x78)};
                    std::memcpy(f, w, sizeof(f));
                    char line[200];
                    std::snprintf(line, sizeof(line), "  # player %06x hp=%.3f pos=%.3f,%.3f,%.3f h=%.4f\n", a, f[0], f[1], f[2], f[3], f[4]);
                    dumpText += line;
                }
        }
        h = simDigest(ram);
        std::fprintf(g_stateLog, "%u %u %u %016llx rng=%08x\n", frame, n, chars, static_cast<unsigned long long>(h),
                     rngAddr() ? rd32(ram, rngAddr()) : 0u);
        if (dump)
            std::fputs(dumpText.c_str(), g_stateLog);
    }

    // KZ_STREAM_LOG=1 (with KZ_STATE_LOG, ES only): every frame, the stream manager's entries ([0x559DF0], +0x94 count,
    // +0x9c array of 0x38-byte entries, word 0 = state; ES FUN_00188fb0/FUN_00189f20) whose state changed:
    // "  S f=<frame> e=<index> <old>-><new> res=<entry+0x24> vt=<[res]> w1=<+4> w8=<+0x20> wb=<+0x2c>".
    std::vector<uint32_t> g_streamPrev;
    void logStream(const uint8_t *ram, uint32_t frame)
    {
        if (!kzIsEs())
            return;
        const uint32_t mgr = rd32(ram, 0x00559DF0u);
        if (mgr < 0x100000u || mgr >= kRamSize)
            return;
        const uint32_t n = rd32(ram, mgr + 0x94), arr = rd32(ram, mgr + 0x9c);
        if (n > 4096u || arr < 0x100000u || arr >= kRamSize)
            return;
        g_streamPrev.resize(n, 0xFFFFFFFFu);
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t e = arr + i * 0x38, st = rd32(ram, e);
            if (st != g_streamPrev[i])
            {
                const uint32_t res = rd32(ram, e + 0x24);
                std::fprintf(g_stateLog, "  S f=%u e=%u %d->%d res=%x vt=%x w1=%x w8=%x wb=%x\n", frame, i,
                             static_cast<int>(g_streamPrev[i]), static_cast<int>(st), res,
                             res >= 0x100000u && res < kRamSize ? rd32(ram, res) : 0u, rd32(ram, e + 4), rd32(ram, e + 0x20),
                             rd32(ram, e + 0x2c));
                g_streamPrev[i] = st;
            }
        }
    }

    // State digest compared between the two PCs (kz_netplay): random state + every character's health, position and
    // heading (memory scan of character entities, in address order; identical heaps on both PCs give the same order).
    uint64_t simDigest(const uint8_t *ram)
    {
        // Random state + position and heading of every character controller (ctlVtable objects move the characters).
        // Entities are not scanned: weapons and first-person objects keep their owner's controller at +0x31C too and
        // their state depends on which player a PC shows.
        uint64_t h = 0xCBF29CE484222325ull;
        auto mix = [&h](uint32_t w) { h = (h ^ w) * 0x100000001B3ull; };
        if (rngAddr())
            mix(rd32(ram, rngAddr()));
        const uint32_t ctlVt = kzAddrs().ctlVtable;
        for (uint32_t a = 0x100000; a + 0x80 < kRamSize; a += 4)
        {
            if (rd32(ram, a) != ctlVt)
                continue;
            mix(rd32(ram, a + 0x50));
            mix(rd32(ram, a + 0x54));
            mix(rd32(ram, a + 0x58));
            mix(rd32(ram, a + 0x78));
        }
        return h;
    }

    void limiterWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (kzCoopResume(rdram, ctx))
        {
            // back from a Lua call started below in this same frame (kz_coop): run the real limiter now
            if (g_lockstep)
                if (const uint32_t o = getRegU32(ctx, 4))
                    wr32(rdram, o + 0x68, rd32(rdram, kzAddrs().vsyncCounter) - g_step);
            g_orig(rdram, ctx, rt);
            return;
        }
        const uint32_t frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
        if (rt)
            g_ps2LastGameFrameTick.store(rt->eeScheduler().currentVSyncTick(), std::memory_order_relaxed);
        g_ps2InMovie.store(false, std::memory_order_relaxed);
        g_countChecks.store(0, std::memory_order_relaxed);
        // KZ_FRAME_SPIKES=<ms>: log every game frame whose wall time (limiter to limiter, before pacing) exceeds <ms>
        static const double spikeMs = std::getenv("KZ_FRAME_SPIKES") ? std::atof(std::getenv("KZ_FRAME_SPIKES")) : 0.0;
        if (spikeMs > 0.0)
        {
            static auto last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(now - last).count();
            if (frame > 2 && ms > spikeMs)
                std::printf("[kz] slow frame %u: %.1f ms\n", frame - 1, ms);
            last = now;
        }
        kzFramePace();
        // Emulated clock per game frame (KZ_LOCKSTEP_CLOCK=0: off). With PS2X_WALLCLOCK=0 the EE cycle clock only
        // advances by the cycles the recompiled code charges (~0.18x real time in a level), while the lockstep frame
        // stands for g_step vblanks: everything on that clock ran slow (IOP time, so the SPU2 made 18% of the audio
        // it should: crackling sound in co-op; vblank-timed waits). Each frame now spans at least g_step vblank
        // periods of EE cycles. Deterministic: the same cycle count at the same point of the frame on every PC.
        static const bool clockSync = !(std::getenv("KZ_LOCKSTEP_CLOCK") && *std::getenv("KZ_LOCKSTEP_CLOCK") == '0');
        if (g_lockstep && clockSync && rt)
        {
            static uint64_t lastCycle = 0;
            static uint32_t lastFrame = 0;
            EeScheduler &sched = rt->eeScheduler();
            const uint64_t per = static_cast<uint64_t>(g_step) * EeScheduler::kEeClockHz * ps2VblankPeriodMicros() / 1000000ull;
            uint64_t now = sched.eeCycleForTrace();
            static const bool clockLog = std::getenv("KZ_CLOCK_LOG") != nullptr;
            if (clockLog && frame % 50 == 0)
                std::fprintf(stderr, "[kz] clock f=%u last=%u now=%llu delta=%lld per=%llu\n", frame, lastFrame,
                             static_cast<unsigned long long>(now), static_cast<long long>(now - lastCycle),
                             static_cast<unsigned long long>(per));
            // Cumulative target (11-05): frame N of a run of consecutive frames sits at base + N * per. The old per-frame
            // floor never gave back the excess of heavy frames, so emulated time ran ~1.1x real time at 60 fps and the
            // SPU2 made 10 % too much sound (the output dropped pieces: crackling, mostly in voices).
            // KZ_LOCKSTEP_CLOCK=floor: the old per-frame floor.
            static const bool floorMode = std::getenv("KZ_LOCKSTEP_CLOCK") && _stricmp(std::getenv("KZ_LOCKSTEP_CLOCK"), "floor") == 0;
            static uint64_t runBase = 0;
            static uint32_t runFrame0 = 0;
            if (!(lastFrame && frame == lastFrame + 1 && now >= lastCycle))
            {
                runBase = now;
                runFrame0 = frame;
            }
            const uint64_t target = runBase + static_cast<uint64_t>(frame - runFrame0) * per;
            if (!floorMode && lastFrame && frame == lastFrame + 1 && now >= lastCycle && now < target)
            {
                uint64_t add = target - now;
                while (add)
                {
                    const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(add, 1u << 20));
                    sched.accountCycles(step);
                    add -= step;
                }
                now = sched.eeCycleForTrace();
            }
            else if (floorMode && lastFrame && frame == lastFrame + 1 && now >= lastCycle && now - lastCycle < per)
            {
                uint64_t add = per - (now - lastCycle);
                while (add)
                {
                    const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(add, 1u << 20));
                    sched.accountCycles(step);
                    add -= step;
                }
                now = sched.eeCycleForTrace();
            }
            lastCycle = now;
            lastFrame = frame;
        }
        // KZ_POS_LOG=N: every N game frames, the position of every character controller (RE / speed checks).
        static const uint32_t posEvery = std::getenv("KZ_POS_LOG") ? static_cast<uint32_t>(std::atoi(std::getenv("KZ_POS_LOG"))) : 0u;
        if (posEvery && frame % posEvery == 0)
        {
            std::string line = "[kz] pos f=" + std::to_string(frame) + " t=" +
                               std::to_string(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
            const uint32_t ctlVt = kzAddrs().ctlVtable;
            for (uint32_t a = 0x100000; a + 0x80 < kRamSize; a += 4)
                if (rd32(rdram, a) == ctlVt)
                {
                    float p[3], hd;
                    std::memcpy(p, rdram + a + 0x50, 12);
                    std::memcpy(&hd, rdram + a + 0x78, 4);
                    char b[112];
                    std::snprintf(b, sizeof(b), " %x:(%.2f,%.2f,%.2f,h%.2f)", a, p[0], p[1], p[2], hd);
                    line += b;
                }
            std::cout << line << std::endl;
        }
        if (frame == 1)
        {
            countStartLevel(rdram);
            // KZ_LOCKSTEP_SEED (hex, default 1234567): every peer starts the level with the same random sequence.
            if (g_lockstep && rngAddr())
            {
                const char *sd = std::getenv("KZ_LOCKSTEP_SEED");
                wr32(rdram, rngAddr(), sd ? static_cast<uint32_t>(std::strtoul(sd, nullptr, 16)) : 0x01234567u);
            }
        }
        const uint32_t obj = getRegU32(ctx, 4); // a0: the game object's frame timer
        if (g_hashLog && frame >= g_hashFrom && (frame - g_hashFrom) % g_hashEvery == 0)
            logHash(rdram, frame, obj ? rd32(rdram, obj + 0x64) : 0);
        if (g_stateLog)
        {
            logState(rdram, frame);
            static const bool streamLog = std::getenv("KZ_STREAM_LOG") != nullptr;
            if (streamLog)
                logStream(rdram, frame);
        }
        // KZ_RAM_DUMP=<frame>:<path>: write the 32 MB of EE RAM at that frame (limiter entry) for an offline diff.
        // KZ_SCHED_TRACE=<from>,<to>:<path>: scheduler trace (thread runs, invocations, events with EE cycles) between
        // those game frames, to diff two runs.
        static const char *schedTrace = std::getenv("KZ_SCHED_TRACE");
        if (schedTrace)
        {
            const uint32_t from = static_cast<uint32_t>(std::atoi(schedTrace));
            const char *comma = std::strchr(schedTrace, ',');
            const char *colon = std::strchr(schedTrace, ':');
            const uint32_t to = comma ? static_cast<uint32_t>(std::atoi(comma + 1)) : from + 1;
            if (frame == from && colon)
                g_ps2SchedTrace = std::fopen(colon + 1, "w");
            if (g_ps2SchedTrace)
                std::fprintf(g_ps2SchedTrace, "frame %u\n", frame);
            if (frame == to && g_ps2SchedTrace)
            {
                std::fclose(g_ps2SchedTrace);
                g_ps2SchedTrace = nullptr;
            }
        }
        // Several frames: KZ_RAM_DUMP=<f1>,<f2>,...:<prefix> writes <prefix><frame>.ram for each.
        static const char *ramDump = std::getenv("KZ_RAM_DUMP");
        if (ramDump)
            if (const char *c = std::strchr(ramDump, ':'))
                for (const char *p = ramDump; p < c; ++p)
                {
                    if (static_cast<uint32_t>(std::atoi(p)) == frame)
                    {
                        const std::string path = std::string(c + 1) + std::to_string(frame) + ".ram";
                        if (std::FILE *f = std::fopen(path.c_str(), "wb"))
                        {
                            std::fwrite(rdram, 1, kRamSize, f);
                            std::fclose(f);
                        }
                        break;
                    }
                    while (p < c && *p != ',')
                        ++p;
                }
        if (g_stopAt && frame > g_stopAt)
        {
            if (g_hashLog)
                std::fflush(g_hashLog);
            if (g_stateLog)
                std::fflush(g_stateLog);
            std::cout << "[kz] lockstep: KZ_HASH_STOP reached at frame " << frame << std::endl;
            std::fflush(stdout);
            std::_Exit(0);
        }
        kzNetFrame(rdram, frame);
        if (const uint32_t every = kzNetDigestEvery(); every && frame % every == 0)
            kzNetSetDigest(frame, simDigest(rdram));
        if (g_lockstep && obj)
        {
            // The original computes elapsed = (vsyncCounter >> s) - [obj+0x68] (s = 0 with the port's timer patches) and loops
            // until it is >= 1. Make it exactly g_step.
            const uint32_t now = rd32(rdram, kzAddrs().vsyncCounter);
            wr32(rdram, obj + 0x68, now - g_step);
        }
        if (kzCoopFrame(rdram, ctx, rt, frame))
            return; // the guest runs Lua first and comes back here (kzCoopResume)
        g_orig(rdram, ctx, rt);
    }

    // KZ_IO_LOG=1 (with KZ_STATE_LOG): logs every async file read request (kzAddrs().fileReadAsync, US FUN_0014d790) and
    // every poll that reports completion (filePoll, US FUN_0014dad0: *a2 = done), with the game frame, into the state log
    // ("  I f=<frame> read h=<handle> buf=<a1> len=<a2>" / "  I f=<frame> done h=<handle>").
    PS2Runtime::RecompiledFunction g_origRead = nullptr, g_origPoll = nullptr;
    void readWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5), a2 = getRegU32(ctx, 6);
        if (g_stateLog)
            std::fprintf(g_stateLog, "  I f=%u read h=%x buf=%x len=%x ra=%x\n", g_frame.load(std::memory_order_relaxed),
                         rd32(rdram, a0 + 0xC), a1, a2, getRegU32(ctx, 31));
        g_origRead(rdram, ctx, rt);
    }
    void pollWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t a0 = getRegU32(ctx, 4), a2 = getRegU32(ctx, 6), ra = getRegU32(ctx, 31);
        g_origPoll(rdram, ctx, rt);
        if (g_stateLog && ctx->pc == ra && rdram[a2 & kRamMask] != 0)
            std::fprintf(g_stateLog, "  I f=%u done h=%x\n", g_frame.load(std::memory_order_relaxed), rd32(rdram, a0 + 0xC));
    }

    // KZ_LOCKSTEP_COUNT=1 (default on with KZ_LOCKSTEP): COP0 Count (the game's millisecond clock, ES FUN_00146770 =
    // Count / 0x48000) follows game frames instead of the host clock: frame * (294912000 / 60) cycles, plus 256 cycles per
    // scheduler checkpoint since the frame started (capped below one frame), so guest spin-waits on Count still end.
    std::atomic<uint32_t> g_countBase{0};   // virtual Count at game frame 1
    std::atomic<bool> g_countVirtual{false};
    std::atomic<uint32_t> g_countLast{0};
    uint32_t countHook(uint64_t eeCycle)
    {
        // One game frame = g_step guest vblanks at the session's vblank rate (60 Hz: 294912000 / 60 per vblank).
        const uint32_t kPerFrame = 294912000u * g_step / static_cast<uint32_t>(kzTimingRate());
        if (!g_countVirtual.load(std::memory_order_acquire))
        {
            // Front end and loading: the EE cycle clock (deterministic with PS2X_WALLCLOCK=0 and the synchronous IOP).
            const uint32_t v = static_cast<uint32_t>(eeCycle);
            g_countLast.store(v, std::memory_order_relaxed);
            return v;
        }
        const uint32_t n = g_countChecks.fetch_add(1, std::memory_order_relaxed);
        // Not capped: guest waits of several ms inside one frame (stream manager FUN_0018ea40) must be able to end.
        const uint64_t within = static_cast<uint64_t>(n) * 256u;
        const uint32_t f = g_frame.load(std::memory_order_relaxed);
        return g_countBase.load(std::memory_order_relaxed) +
               static_cast<uint32_t>(static_cast<uint64_t>(f > 0 ? f - 1 : 0) * kPerFrame + within);
    }
    // At game frame 1 (guest thread): switch Count to the virtual clock and zero the game's millisecond accumulator
    // (64-bit at kzAddrs().vsyncCountAcc, last Count at vsyncCountLast; ES FUN_00146770), so every peer starts the level
    // with the same clock.
    void countStartLevel(uint8_t *rdram)
    {
        if (g_ps2Cop0CountHook != &countHook || g_countVirtual.load())
            return;
        const uint32_t base = g_countLast.load(std::memory_order_relaxed);
        g_countBase.store(base, std::memory_order_relaxed);
        const uint64_t zero = 0;
        std::memcpy(rdram + (kzAddrs().vsyncCountAcc & kRamMask), &zero, 8);
        wr32(rdram, kzAddrs().vsyncCountLast, base);
        g_countVirtual.store(true, std::memory_order_release);
        std::cout << "[kz] lockstep: COP0 Count now follows game frames (game clock reset at frame 1)" << std::endl;
    }

    // KZ_RNG_LOG=1 (with KZ_STATE_LOG, ES only): every call of the random functions (ES FUN_00140580 int,
    // FUN_001405b0 float) with the caller and the state before: "  R f=<frame> <fn> ra=<ra> st=<state>".
    PS2Runtime::RecompiledFunction g_origRndI = nullptr, g_origRndF = nullptr;
    void rndLog(uint8_t *rdram, R5900Context *ctx, char fn)
    {
        if (g_stateLog)
            std::fprintf(g_stateLog, "  R f=%u %c ra=%x st=%08x\n", g_frame.load(std::memory_order_relaxed), fn,
                         getRegU32(ctx, 31), rd32(rdram, 0x0055B300u));
    }
    void rndIWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt) { rndLog(rdram, ctx, 'i'); g_origRndI(rdram, ctx, rt); }
    void rndFWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt) { rndLog(rdram, ctx, 'f'); g_origRndF(rdram, ctx, rt); }

    // KZ_RNGW_LOG=1 (with KZ_STATE_LOG, ES only): wraps every function that stores to the random state (scan of sw/sd
    // with %lo(0x55B300)) and logs the ones that changed it: "  W f=<frame> fn=<addr> ra=<ra> sp=<sp> d=<depth> <before>-><after>".
    constexpr uint32_t kRngWriters[] = {0x140ef0u,0x1a41acu,0x1c688cu,0x1c6a00u,0x1c7d58u,0x1e838cu,0x1e83e8u,0x1e879cu,0x1e88e4u,
        0x1e8b20u,0x1f3b70u,0x21b648u,0x21ebacu,0x222a8cu,0x22551cu,0x22825cu,0x228f00u,0x2290c0u,0x229348u,0x23c384u,0x274d38u,
        0x278144u,0x2782acu,0x279168u,0x280140u,0x2848f0u,0x28a620u,0x28a6bcu,0x28c260u,0x28c5e8u,0x2f56c8u,0x318740u,0x31ed40u,
        0x31eeecu,0x31ef7cu,0x31eff0u,0x31f090u,0x31f144u,0x31f444u,0x31f5c0u,0x31f678u,0x31fa48u,0x320148u,0x3201f8u,0x320644u,
        0x3c8338u,0x3c83d8u,0x3c8830u,0x3c8a08u,0x3fa5a0u,0x4abda8u};
    constexpr size_t kRngWriterCount = sizeof(kRngWriters) / sizeof(kRngWriters[0]);
    PS2Runtime::RecompiledFunction g_rngwOrig[kRngWriterCount] = {};
    thread_local int g_rngwDepth = 0;
    template <size_t I>
    void rngwWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t before = rd32(rdram, 0x0055B300u), ra = getRegU32(ctx, 31), sp = getRegU32(ctx, 29);
        ++g_rngwDepth;
        g_rngwOrig[I](rdram, ctx, rt);
        --g_rngwDepth;
        const uint32_t after = rd32(rdram, 0x0055B300u);
        if (after != before && g_stateLog)
            std::fprintf(g_stateLog, "  W f=%u fn=%x ra=%x sp=%x d=%d %08x->%08x\n", g_frame.load(std::memory_order_relaxed),
                         kRngWriters[I], ra, sp, g_rngwDepth, before, after);
    }
    template <size_t... I>
    void installRngw(PS2Runtime &runtime, std::index_sequence<I...>)
    {
        ((g_rngwOrig[I] = runtime.lookupFunction(kRngWriters[I]),
          (g_rngwOrig[I] && runtime.replaceFunction(kRngWriters[I], &rngwWrap<I>)) ||
              (std::cerr << "[kz] rngw hook failed @" << std::hex << kRngWriters[I] << std::dec << std::endl, false)),
         ...);
    }

    PS2Runtime *g_runtime = nullptr;
    // Deterministic runs: KZ_INPUT_SCRIPT times count vblanks (cycle-timed with PS2X_DETERMINISTIC), not wall time.
    double vblankClock() { return g_runtime ? static_cast<double>(g_runtime->eeScheduler().currentVSyncTick()) / 60.0 : 0.0; }
    double frameClock() { return static_cast<double>(g_frame.load(std::memory_order_relaxed)) * g_step / 60.0; }
}

bool kzLockstepEnabled() { return g_lockstep; }
void kzLockstepSetStep(uint32_t step) { g_step = step >= 2 ? 2u : 1u; }
uint32_t kzLockstepStep() { return g_step; }
uint32_t kzLockstepFrame() { return g_frame.load(std::memory_order_relaxed); }

void kzLockstepInstall(PS2Runtime &runtime)
{
    const char *ls = std::getenv("KZ_LOCKSTEP");
    g_lockstep = ls && *ls && *ls != '0';
    // A netplay session (or its local tests) needs the fixed step.
    for (const char *v : {"KZ_NET", "KZ_NET_LATCH", "KZ_NET_P2SCRIPT"})
        if (const char *e = std::getenv(v); e && *e && *e != '0')
            g_lockstep = true;
    g_runtime = &runtime;
    kzCoopInstall(runtime);
    const char *hl = std::getenv("KZ_HASH_LOG");
    if (const char *e = std::getenv("KZ_HASH_EVERY"); e && std::atoi(e) > 0)
        g_hashEvery = static_cast<uint32_t>(std::atoi(e));
    if (const char *e = std::getenv("KZ_HASH_FROM"))
        g_hashFrom = static_cast<uint32_t>(std::atoi(e));
    if (const char *e = std::getenv("KZ_HASH_STOP"))
        g_stopAt = static_cast<uint32_t>(std::atoi(e));
    const char *sl = std::getenv("KZ_STATE_LOG");
    if (const char *e = std::getenv("KZ_STATE_DUMP"))
        g_stateDump = static_cast<uint32_t>(std::atoi(e));
    if (!g_lockstep && !(hl && *hl) && !(sl && *sl) && !g_stopAt)
        return;
    if (sl && *sl)
        g_stateLog = std::fopen(sl, "w");
    if (hl && *hl)
    {
        g_hashLog = std::fopen(hl, "w");
        if (!g_hashLog)
            std::cerr << "[kz] lockstep: cannot open KZ_HASH_LOG " << hl << std::endl;
    }
    if (g_lockstep)
    {
        const char *lc = std::getenv("KZ_LOCKSTEP_COUNT");
        if (!lc || *lc != '0')
            g_ps2Cop0CountHook = &countHook;
    }
    g_orig = runtime.lookupFunction(kzAddrs().frameLimiter);
    if (!g_orig || !runtime.replaceFunction(kzAddrs().frameLimiter, &limiterWrap))
    {
        std::cerr << "[kz] lockstep: frame limiter hook failed" << std::endl;
        return;
    }
    // KZ_LOCKSTEP_SCRIPTCLOCK=1: KZ_INPUT_SCRIPT times count game frames (t = frame / 60). The front end does not pass the
    // limiter (frame stays 0 there), so menu inputs need the default vblank clock.
    if (const char *sc = std::getenv("KZ_LOCKSTEP_SCRIPTCLOCK"); sc && *sc == '1')
        kzInputSetScriptClock(&frameClock);
    else if (const char *det = std::getenv("PS2X_DETERMINISTIC"); det && *det == '1')
        kzInputSetScriptClock(&vblankClock);
    kzNetSetClock(&vblankClock);
    if (const char *rl = std::getenv("KZ_RNG_LOG"); rl && *rl == '1' && kzIsEs())
    {
        g_origRndI = runtime.lookupFunction(0x00140580u);
        g_origRndF = runtime.lookupFunction(0x001405b0u);
        if (!g_origRndI || !g_origRndF || !runtime.replaceFunction(0x00140580u, &rndIWrap) ||
            !runtime.replaceFunction(0x001405b0u, &rndFWrap))
            std::cerr << "[kz] lockstep: rng log hooks failed" << std::endl;
    }
    if (const char *rw = std::getenv("KZ_RNGW_LOG"); rw && *rw == '1' && kzIsEs())
        installRngw(runtime, std::make_index_sequence<kRngWriterCount>{});
    if (const char *io = std::getenv("KZ_IO_LOG"); io && *io == '1')
    {
        g_origRead = runtime.lookupFunction(kzAddrs().fileReadAsync);
        g_origPoll = runtime.lookupFunction(kzAddrs().filePoll);
        if (!g_origRead || !g_origPoll || !runtime.replaceFunction(kzAddrs().fileReadAsync, &readWrap) ||
            !runtime.replaceFunction(kzAddrs().filePoll, &pollWrap))
            std::cerr << "[kz] lockstep: io log hooks failed" << std::endl;
    }
    std::cout << "[kz] lockstep: fixed step " << (g_lockstep ? "on" : "off") << ", hash log "
              << (g_hashLog ? hl : "off") << " (every " << g_hashEvery << " frames)" << std::endl;
}
