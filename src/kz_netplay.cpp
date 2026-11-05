// Fork (KZ1 co-op): input lockstep between two PCs. See kz_netplay.h.
//
// Configuration (environment; the launcher sets the same variables):
//   KZ_NET=host|join          start a session (host waits for the joiner, joiner connects to KZ_NET_ADDR)
//   KZ_NET_ADDR=ip[:port]     joiner: the host's address. KZ_NET_PORT: host's UDP port (default 47474)
//   KZ_NET_DELAY=n            input delay in game frames (default 3; host decides, sent to the joiner)
//   KZ_NET_DIGEST=n           compare a state digest with the peer every n frames (default 60, 0 = off)
//   KZ_NET_TIMEOUT=s          give up waiting for the peer after s seconds (default 15): the session ends, solo play goes on
//   KZ_NET_LATCH=1            test without network: latch the local input per frame exactly as a session does
//   KZ_NET_P2SCRIPT="a-b:btn;a-b:ly=-1;..."  test without network: player 1's input from a script in game frames
//   KZ_NET_LOG=path           session log (handshake, stalls, digests)
//
// Protocol (UDP, little endian, every packet starts with magic 'KZ1C' and a type byte):
//   HELLO   (1): version u32, elf crc u32                 joiner -> host, repeated until WELCOME
//   WELCOME (2): version u32, delay u8                    host -> joiner
//   INPUT   (3): ack u32 (first frame still missing from the peer), first u32, count u8, count x 6 input bytes,
//                digest frame u32, digest u64            both ways, every frame and while waiting; the inputs not yet
//                acknowledged are resent in each packet, so a lost packet costs nothing but the next one's arrival.
//   BYE     (4)
#include "kz_netplay.h"
#include "kz_lockstep.h"
#include "kz_timing.h"
#include "kz_gs.h"
#include "runtime/ps2_host_gs.h"

#include "kz_input.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern void (*g_ps2VBlankGate)(uint64_t nextTick); // EeScheduler.cpp (fork)

extern std::atomic<bool> g_ps2InMovie; // EeScheduler.cpp (fork)
extern std::atomic<const char *> g_ps2EeWait; // ps2_vif1_worker.cpp (fork): freeze diagnosis

namespace
{
    constexpr uint32_t kMagic = 0x43315A4Bu; // "KZ1C"
    // 2: inputs carry mouse motion + keyboard/mouse flag (KzNetInput::kWireSize)
    // 3: HELLO carries player 2's character, host's memory card sent at connect, menu input stream
    constexpr uint32_t kVersion = 6; // v6 (0.3.1): PathResource guard at level unload; v5 (0.3.0): native player 2 (created at OnInitialize); v4: WELCOME carries the frame rate
    // Session frame rate (KZ_NET_FPS on the host; the joiner takes the host's from WELCOME): 30 = the console's pace
    // (60 Hz vblank, 2 vblanks per game frame), else N game frames per second (N Hz vblank, 1 per frame).
    uint32_t g_sessionFps = 30;
    bool g_menuBannerOn = false; // joiner: the "player 1 drives the menus" banner is up (cleared at the next game frame)
    double g_paceHz = -1.0; // kzFramePace: < 0 = KZ_FRAME_PACE
    enum : uint8_t { kHello = 1, kWelcome = 2, kInput = 3, kBye = 4, kMenu = 5, kMenuAck = 6, kMcReq = 7, kMcData = 8 };
    constexpr uint32_t kMcChunk = 1024;

    enum class Mode { Off, Host, Join, Latch };
    Mode g_mode = Mode::Off;
    std::atomic<bool> g_connected{false};
    bool g_sessionOver = false;
    uint32_t g_delay = 3;
    uint32_t g_digestEvery = 60;
    double g_timeout = 15.0;
    std::FILE *g_log = nullptr;

    SOCKET g_sock = INVALID_SOCKET;
    sockaddr_in g_peer{};
    bool g_havePeer = false;
    std::mutex g_sockMutex; // the handshake thread and the guest thread share the socket

    // Inputs by game frame. local: frames this PC sampled (frame + delay); remote: received from the peer.
    std::map<uint32_t, KzNetInput> g_local, g_remote;
    uint32_t g_remoteNext = 1; // first frame not yet received from the peer (contiguous)
    uint32_t g_peerAck = 1;    // first frame the peer still misses from us
    uint32_t g_frame = 0;
    KzNetInput g_latched[2];
    std::map<uint32_t, uint64_t> g_myDigest, g_peerDigest;
    uint32_t g_lastSentDigestFrame = 0;
    uint64_t g_lastSentDigest = 0;
    bool g_desyncReported = false;
    std::atomic<double> g_lastRecv{0.0}; // last packet from the peer (nowSec)

    // Player 2's character (joiner's KZ_COOP_CHAR, sent in HELLO): 0 Templar, 1 Luger, 2 Rico (Choyan), 3 Hakha.
    uint32_t g_p2Char = 0;

    // Menu input stream. Menus, loading, the end-of-mission screens and the pause menu run outside the frame limiter;
    // there the host's pad drives both PCs: every pad-0 read the game does outside a game frame (all reads before the
    // first frame, and every read but the first one after a frame) takes the next entry. The host appends its local pad,
    // the joiner waits for and replays the host's entry with the same index. The read sequence is deterministic (same
    // count in two runs), so both games see the same menu input at the same read.
    //
    // Keyed by vblank time, not by read count: the menus play videos decoded on the host PC, so how many times the game
    // reads the pad per vblank depends on the PC's speed (two instances diverged after ~870 reads), while the menu logic
    // follows vblank time (the old Cross-every-5-vblank-seconds autostart always reached identical games). A menu phase
    // starts at the first menu read after game frame N (phase N; 0 = boot) and its time is counted in vblanks from there.
    // The host sends every change of its pad as (phase, tick, input) plus its progress (phase, tick); the joiner, at
    // (phase, tick), waits until the host is past that point and uses the host's last change at or before it.
    struct MenuChange
    {
        uint32_t phase, tick;
        KzNetInput in;
    };
    std::vector<MenuChange> g_menuHist;            // host: every change so far; joiner: every change received (contiguous)
    std::map<uint32_t, MenuChange> g_menuOoo;      // joiner: changes received ahead of a gap
    uint32_t g_menuSeq = 0;                        // reads (log)
    uint32_t g_menuAcked = 0;                      // host: changes the joiner has
    uint64_t g_hostProgress = 0;                   // joiner: host's (phase << 32 | tick); host: its own
    uint32_t g_phase = 0xFFFFFFFFu, g_phaseTick0 = 0; // current menu phase on this PC and its start vblank
    double g_menuLastSend = 0.0;
    uint32_t (*g_vtick)() = nullptr;               // vblank count (kz_lockstep)

    // Host's memory card (KZ_MC_ROOT and its "_slot1" sibling) as it was at start-up, sent to the joiner before its game
    // boots: both games must read the same card. Blob: per file u8 card (0, 1), u16 path length, path, u32 size, data.
    std::vector<uint8_t> g_mcBlob;
    std::map<uint32_t, std::vector<uint8_t>> g_mcChunks; // joiner: received chunks
    uint32_t g_mcTotal = 0;                              // joiner: blob size (from the first chunk)

    void logf(const char *fmt, ...)
    {
        if (!g_log)
            return;
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(g_log, fmt, ap);
        va_end(ap);
        std::fputc('\n', g_log);
        std::fflush(g_log);
    }

    double nowSec()
    {
        static const auto t0 = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    // ---- test script for player 1 (KZ_NET_P2SCRIPT), in game frames
    struct P2Item
    {
        uint32_t from, to;
        uint16_t mask;
        int axis; // 0 none, 1 rx, 2 ry, 3 lx, 4 ly, 5 mx, 6 my (mouse counts per frame, with kbm flag)
        float value;
    };
    std::vector<P2Item> g_p2script;
    bool g_p2scripted = false;
    void parseP2Script(const char *e)
    {
        static const struct { const char *n; uint16_t m; } names[] = {
            {"cross", KZ_PAD_CROSS}, {"circle", KZ_PAD_CIRCLE}, {"square", KZ_PAD_SQUARE}, {"triangle", KZ_PAD_TRIANGLE},
            {"l1", KZ_PAD_L1}, {"r1", KZ_PAD_R1}, {"l2", KZ_PAD_L2}, {"r2", KZ_PAD_R2}, {"l3", KZ_PAD_L3}, {"r3", KZ_PAD_R3},
            {"start", KZ_PAD_START}, {"select", KZ_PAD_SELECT}, {"up", KZ_PAD_UP}, {"down", KZ_PAD_DOWN},
            {"left", KZ_PAD_LEFT}, {"right", KZ_PAD_RIGHT}};
        std::string all(e);
        size_t pos = 0;
        while (pos < all.size())
        {
            size_t end = all.find(';', pos);
            std::string it = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? all.size() : end + 1;
            unsigned a = 0, b = 0;
            char what[32] = {};
            if (std::sscanf(it.c_str(), "%u-%u:%31s", &a, &b, what) != 3)
                continue;
            P2Item p{a, b, 0, 0, 0.0f};
            if (char *eq = std::strchr(what, '='))
            {
                *eq = 0;
                const char *ax[] = {"", "rx", "ry", "lx", "ly", "mx", "my"};
                for (int i = 1; i <= 6; ++i)
                    if (_stricmp(what, ax[i]) == 0)
                        p.axis = i;
                p.value = static_cast<float>(std::atof(eq + 1));
            }
            else
                for (const auto &n : names)
                    if (_stricmp(n.n, what) == 0)
                        p.mask = n.m;
            if (p.mask || p.axis)
                g_p2script.push_back(p);
        }
        g_p2scripted = !g_p2script.empty();
    }
    KzNetInput p2ScriptInput(uint32_t frame)
    {
        KzNetInput in;
        uint16_t pressed = 0;
        for (const P2Item &p : g_p2script)
        {
            if (frame < p.from || frame > p.to)
                continue;
            pressed |= p.mask;
            if (p.axis >= 5)
            {
                (p.axis == 5 ? in.mdx : in.mdy) = static_cast<int16_t>(p.value * 8.0f);
                in.flags |= 1u;
            }
            else if (p.axis)
            {
                const int v = static_cast<int>(128.0f + p.value * 127.0f);
                in.b[1 + p.axis] = static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
            }
        }
        const uint16_t low = static_cast<uint16_t>(~pressed);
        in.b[0] = static_cast<uint8_t>(low & 0xFF);
        in.b[1] = static_cast<uint8_t>(low >> 8);
        return in;
    }

    KzNetInput sampleLocal()
    {
        uint8_t data[32];
        KzNetInput in;
        if (kzPadProvider(0, 0, data, sizeof(data)))
            std::memcpy(in.b, data + 2, 6);
        // Online sessions: no pause (KZ_NET_PAUSE=1 allows it). The pause menu does not run the game frame, so inputs
        // stop being exchanged there and each PC's menu would act on its own pad (desync, and the other PC freezes).
        // Since the menu input stream the pause menu is driven by the host's pad on both PCs: the host may pause
        // (KZ_NET_PAUSE=0 forbids it), the joiner's START never travels (its pad 1 would pause both games).
        static const bool allowPause = !(std::getenv("KZ_NET_PAUSE") && std::getenv("KZ_NET_PAUSE")[0] == '0');
        if (g_mode == Mode::Join || (!allowPause && g_mode == Mode::Host))
        {
            const uint16_t raw = static_cast<uint16_t>(in.b[0] | (in.b[1] << 8)) | KZ_PAD_START; // active low: set = released
            in.b[0] = static_cast<uint8_t>(raw & 0xFF);
            in.b[1] = static_cast<uint8_t>(raw >> 8);
        }
        // SELECT (keyboard Tab) opens the objectives screen, which froze co-op sessions (11-05, Santi): it never travels
        // in a session, on either PC (KZ_NET_SELECT=1 allows it).
        static const bool allowSelect = std::getenv("KZ_NET_SELECT") && std::getenv("KZ_NET_SELECT")[0] == '1';
        if (!allowSelect && (g_mode == Mode::Join || g_mode == Mode::Host))
        {
            const uint16_t raw = static_cast<uint16_t>(in.b[0] | (in.b[1] << 8)) | KZ_PAD_SELECT;
            in.b[0] = static_cast<uint8_t>(raw & 0xFF);
            in.b[1] = static_cast<uint8_t>(raw >> 8);
        }
        // Mouse motion since the last frame goes with the input (the aim patch reads it from the latched input).
        const KzMouseDelta d = kzInputTakeMouseDelta();
        auto q = [](float v) { const float s = std::round(v * 8.0f); return static_cast<int16_t>(s < -32767.f ? -32767.f : s > 32767.f ? 32767.f : s); };
        in.mdx = q(d.dx);
        in.mdy = q(d.dy);
        in.flags = kzInputUsingKeyboardMouse() ? 1u : 0u;
        return in;
    }

    // ---- socket helpers
    void sendRaw(const std::vector<uint8_t> &pkt)
    {
        if (g_sock == INVALID_SOCKET || !g_havePeer)
            return;
        sendto(g_sock, reinterpret_cast<const char *>(pkt.data()), static_cast<int>(pkt.size()), 0,
               reinterpret_cast<const sockaddr *>(&g_peer), sizeof(g_peer));
    }
    void put32(std::vector<uint8_t> &p, uint32_t v) { for (int i = 0; i < 4; ++i) p.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void put64(std::vector<uint8_t> &p, uint64_t v) { for (int i = 0; i < 8; ++i) p.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    uint32_t get32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
    uint64_t get64(const uint8_t *p) { return get32(p) | (static_cast<uint64_t>(get32(p + 4)) << 32); }
    std::vector<uint8_t> header(uint8_t type)
    {
        std::vector<uint8_t> p;
        put32(p, kMagic);
        p.push_back(type);
        return p;
    }

    void sendMenu(bool force);
    void sendInputs()
    {
        std::vector<uint8_t> p = header(kInput);
        put32(p, g_remoteNext);
        // resend everything the peer has not acknowledged (at most 60 frames)
        uint32_t first = g_peerAck;
        std::vector<const KzNetInput *> list;
        for (auto it = g_local.lower_bound(first); it != g_local.end() && list.size() < 60; ++it)
        {
            if (it->first != first + list.size())
                break;
            list.push_back(&it->second);
        }
        put32(p, first);
        p.push_back(static_cast<uint8_t>(list.size()));
        for (const KzNetInput *in : list)
        {
            uint8_t w[KzNetInput::kWireSize];
            in->write(w);
            p.insert(p.end(), w, w + KzNetInput::kWireSize);
        }
        put32(p, g_lastSentDigestFrame);
        put64(p, g_lastSentDigest);
        sendRaw(p);
        sendMenu(false); // menu entries the joiner still misses (it may still be replaying the host's menus)
    }

    // Host: the menu entries the joiner has not acknowledged yet (at most 100 per packet). Caller holds g_sockMutex.
    void sendMenu(bool force)
    {
        if (g_mode != Mode::Host || !g_connected.load())
            return;
        const double t = nowSec();
        if (!force && t - g_menuLastSend < 0.004)
            return;
        g_menuLastSend = t;
        // MENU (5): progress u64, first u32, count u8, count x (phase u32, tick u32, input)
        std::vector<uint8_t> p = header(kMenu);
        const uint32_t first = std::min<uint32_t>(g_menuAcked, static_cast<uint32_t>(g_menuHist.size()));
        const uint32_t pending = static_cast<uint32_t>(g_menuHist.size()) - first;
        const uint32_t count = std::min<uint32_t>(60, pending);
        // progress: the host's own, or (when not every pending change fits) just before the first change left out
        uint64_t progress = g_hostProgress;
        if (count < pending)
        {
            const MenuChange &next = g_menuHist[first + count];
            progress = ((static_cast<uint64_t>(next.phase) << 32) | next.tick) - 1u;
        }
        put64(p, progress);
        put32(p, first);
        p.push_back(static_cast<uint8_t>(count));
        for (uint32_t i = 0; i < count; ++i)
        {
            const MenuChange &c = g_menuHist[first + i];
            put32(p, c.phase);
            put32(p, c.tick);
            uint8_t w[KzNetInput::kWireSize];
            c.in.write(w);
            p.insert(p.end(), w, w + KzNetInput::kWireSize);
        }
        sendRaw(p);
    }

    void checkDigest(uint32_t frame)
    {
        auto a = g_myDigest.find(frame), b = g_peerDigest.find(frame);
        if (a == g_myDigest.end() || b == g_peerDigest.end())
            return;
        if (a->second != b->second && !g_desyncReported)
        {
            g_desyncReported = true;
            logf("DESYNC at frame %u: mine %016llx peer %016llx", frame, static_cast<unsigned long long>(a->second),
                 static_cast<unsigned long long>(b->second));
            std::cerr << "[kz] netplay: DESYNC detected at game frame " << frame << " (the two PCs no longer simulate the same game)" << std::endl;
        }
        else if (a->second == b->second && frame % (g_digestEvery * 10) == 0)
            logf("digest ok at frame %u", frame);
        g_myDigest.erase(g_myDigest.begin(), g_myDigest.upper_bound(frame));
        g_peerDigest.erase(g_peerDigest.begin(), g_peerDigest.upper_bound(frame));
    }

    // Reads every pending datagram. Returns false on a BYE.
    bool pump()
    {
        if (g_sock == INVALID_SOCKET)
            return true;
        uint8_t buf[1500];
        for (;;)
        {
            sockaddr_in from{};
            int fl = sizeof(from);
            const int n = recvfrom(g_sock, reinterpret_cast<char *>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &fl);
            if (n <= 0)
                return true;
            if (n < 5 || get32(buf) != kMagic)
                continue;
            const uint8_t type = buf[4];
            g_lastRecv = nowSec();
            if (type == kHello && g_mode == Mode::Host && n >= 13)
            {
                if (get32(buf + 5) != kVersion)
                {
                    logf("HELLO with version %u ignored", get32(buf + 5));
                    continue;
                }
                g_peer = from;
                g_havePeer = true;
                if (!g_connected.load())
                    g_p2Char = std::min<uint32_t>(get32(buf + 9), 3u);
                std::vector<uint8_t> w = header(kWelcome);
                put32(w, kVersion);
                w.push_back(static_cast<uint8_t>(g_delay));
                w.push_back(static_cast<uint8_t>(g_sessionFps));
                sendRaw(w);
                if (!g_connected.exchange(true))
                {
                    char ip[64];
                    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                    logf("joiner connected from %s:%u", ip, ntohs(from.sin_port));
                    std::cout << "[kz] netplay: player 2 connected from " << ip << std::endl;
                }
            }
            else if (type == kWelcome && g_mode == Mode::Join && n >= 11)
            {
                g_delay = buf[9];
                g_sessionFps = buf[10];
                if (!g_connected.exchange(true))
                {
                    logf("connected to host, input delay %u", g_delay);
                    std::cout << "[kz] netplay: connected to the host (input delay " << g_delay << " frames)" << std::endl;
                }
            }
            else if (type == kInput && n >= 14 && g_connected.load())
            {
                const uint32_t ack = get32(buf + 5), first = get32(buf + 9);
                const uint8_t count = buf[13];
                if (n < 14 + count * static_cast<int>(KzNetInput::kWireSize) + 12)
                    continue;
                if (ack > g_peerAck)
                    g_peerAck = ack;
                g_local.erase(g_local.begin(), g_local.lower_bound(std::min(g_peerAck, g_frame))); // still needed here until played
                for (uint32_t i = 0; i < count; ++i)
                {
                    KzNetInput in;
                    in.read(buf + 14 + i * KzNetInput::kWireSize);
                    g_remote.emplace(first + i, in);
                }
                while (g_remote.count(g_remoteNext))
                    ++g_remoteNext;
                const uint8_t *d = buf + 14 + count * KzNetInput::kWireSize;
                const uint32_t df = get32(d);
                if (df)
                {
                    g_peerDigest[df] = get64(d + 4);
                    checkDigest(df);
                }
            }
            else if (type == kMenu && g_mode == Mode::Join && n >= 18)
            {
                const uint64_t progress = get64(buf + 5);
                const uint32_t first = get32(buf + 13);
                const uint32_t count = buf[17];
                const size_t before = g_menuHist.size();
                constexpr int kEntry = 8 + static_cast<int>(KzNetInput::kWireSize);
                if (n < 18 + static_cast<int>(count) * kEntry)
                    continue;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t s = first + i;
                    if (s < g_menuHist.size())
                        continue;
                    const uint8_t *e = buf + 18 + i * kEntry;
                    MenuChange c{get32(e), get32(e + 4), {}};
                    c.in.read(e + 8);
                    g_menuOoo[s] = c;
                }
                for (auto it = g_menuOoo.find(static_cast<uint32_t>(g_menuHist.size())); it != g_menuOoo.end();
                     it = g_menuOoo.find(static_cast<uint32_t>(g_menuHist.size())))
                {
                    g_menuHist.push_back(it->second);
                    g_menuOoo.erase(it);
                }
                // the progress counts once every change sent before it is here (the packet holds all of them up to it)
                if (g_menuOoo.empty() && first <= g_menuHist.size() && progress > g_hostProgress)
                    g_hostProgress = progress;
                // ack only when something new arrived (the keep-alive thread acks every 200 ms anyway)
                if (g_menuHist.size() != before)
                {
                    std::vector<uint8_t> a = header(kMenuAck);
                    put32(a, static_cast<uint32_t>(g_menuHist.size()));
                    sendRaw(a);
                }
            }
            else if (type == kMenuAck && g_mode == Mode::Host && n >= 9)
            {
                const uint32_t next = get32(buf + 5);
                if (next > g_menuAcked && next <= g_menuHist.size())
                    g_menuAcked = next;
                // no reply here: answering every ack with a MENU packet (itself acked) was an endless ping-pong that
                // saturated both PCs in menus (pause ran at under 1 vblank per second)
            }
            else if (type == kMcReq && g_mode == Mode::Host && n >= 9)
            {
                // the joiner's HELLO came first (g_peer set); answer one chunk of the start-up memory card snapshot
                const uint32_t idx = get32(buf + 5);
                const uint32_t off = idx * kMcChunk;
                if (off > g_mcBlob.size())
                    continue;
                const uint32_t len = std::min<uint32_t>(kMcChunk, static_cast<uint32_t>(g_mcBlob.size()) - off);
                std::vector<uint8_t> p = header(kMcData);
                put32(p, idx);
                put32(p, static_cast<uint32_t>(g_mcBlob.size()));
                p.insert(p.end(), g_mcBlob.begin() + off, g_mcBlob.begin() + off + len);
                sendRaw(p);
            }
            else if (type == kMcData && g_mode == Mode::Join && n >= 13)
            {
                g_mcTotal = get32(buf + 9);
                g_mcChunks[get32(buf + 5)].assign(buf + 13, buf + n);
            }
            else if (type == kBye)
            {
                logf("peer left");
                std::cout << "[kz] netplay: the other player left" << std::endl;
                return false;
            }
        }
    }

    void endSession(const char *why)
    {
        if (g_sessionOver)
            return;
        g_sessionOver = true;
        logf("session over: %s", why);
        std::cerr << "[kz] netplay: session over (" << why << "), playing on alone" << std::endl;
        sendRaw(header(kBye));
    }

    bool openSocket(uint16_t bindPort)
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return false;
        g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_sock == INVALID_SOCKET)
            return false;
        u_long nb = 1;
        ioctlsocket(g_sock, FIONBIO, &nb);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(bindPort);
        if (bind(g_sock, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0)
        {
            std::cerr << "[kz] netplay: cannot bind UDP port " << bindPort << std::endl;
            return false;
        }
        return true;
    }

    // Handshake until the first game frame: the joiner repeats HELLO, the host answers (from pump). Runs on its own
    // thread so menus are not slowed; stops once the guest thread starts using the socket (frame 1).
    std::atomic<bool> g_handshakeStop{false};
    // Keep-alive every 200 ms for the whole session, both ways (a game may be busy loading for long): the host sends its
    // menu progress, the joiner an ack. Every wait gives up only when nothing at all arrived for KZ_NET_TIMEOUT.
    std::thread g_keepAlive;
    void keepAliveMain()
    {
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::lock_guard lock(g_sockMutex);
            if (g_sessionOver)
                return;
            if (g_mode == Mode::Host)
                sendMenu(true);
            else
            {
                std::vector<uint8_t> a = header(kMenuAck); // joiner: keep-alive (harmless ack)
                put32(a, static_cast<uint32_t>(g_menuHist.size()));
                sendRaw(a);
            }
        }
    }
    std::thread g_handshake;
    void handshakeMain()
    {
        while (!g_handshakeStop.load())
        {
            {
                std::lock_guard lock(g_sockMutex);
                if (g_mode == Mode::Join && !g_connected.load())
                {
                    std::vector<uint8_t> h = header(kHello);
                    put32(h, kVersion);
                    put32(h, g_p2Char);
                    sendRaw(h);
                }
                pump();
                sendMenu(false);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(g_connected.load() ? 10 : 200));
        }
    }
}

// Inputs stay latched for the rest of the level once a session started, also after the other player left: each PC
// keeps driving its own player (the one who left stands still), see kzNetFrame.
std::atomic<bool> g_sessionStarted{false};
bool kzNetActive() { return g_mode != Mode::Off && (g_mode == Mode::Latch || g_sessionStarted.load() || (g_connected.load() && !g_sessionOver)); }
bool kzNetHasPlayer2() { return (g_mode == Mode::Latch && g_p2scripted) || (g_mode != Mode::Latch && g_mode != Mode::Off && g_connected.load() && !g_sessionOver); }
int kzNetLocalPlayer() { return g_mode == Mode::Join ? 1 : 0; }
bool kzNetPeerLeft() { return g_mode != Mode::Latch && g_sessionStarted.load() && g_sessionOver; }
const KzNetInput &kzNetInputOf(int player) { return g_latched[player & 1]; }
uint32_t kzNetDigestEvery() { return g_mode == Mode::Host || g_mode == Mode::Join ? g_digestEvery : 0; }

void kzNetSetDigest(uint32_t frame, uint64_t digest)
{
    g_myDigest[frame] = digest;
    g_lastSentDigestFrame = frame;
    g_lastSentDigest = digest;
    checkDigest(frame);
}

// KZ_AUTOSTART=1 (always on in a session): until the level starts, the local pad is ignored and Cross is pressed for
// 0.2 s every KZ_AUTOSTART_EVERY (default 5) seconds of vblank time. From the title screen that walks Campaign > profile >
// first level > difficulty > character, the same way on both PCs (vblanks are cycle-timed in deterministic mode).
namespace
{
    double (*g_vclock)() = nullptr;
    int g_autostart = -1;
    double g_autoEvery = 5.0;
}
void kzNetSetClock(double (*clock)()) { g_vclock = clock; }

namespace
{
    // KZ_NET_POLLLOG=<file>: per game frame, how many times the game read pad 0 since the previous frame (RE aid for the
    // menu input stream: menus run outside the frame limiter).
    std::FILE *g_pollLog = nullptr;
    uint32_t g_pollsSinceFrame = 0;
    uint64_t g_pollTotal = 0;
}

namespace
{
    bool sessionLive() { return (g_mode == Mode::Host || g_mode == Mode::Join) && !g_sessionOver; }

    // Joiner's menu input at (phase, tick) of the current phase: what the host's game read last at or before that tick.
    // Caller holds g_sockMutex.
    KzNetInput menuInputAt(uint32_t phase, uint32_t rel)
    {
        const uint64_t key = (static_cast<uint64_t>(phase) << 32) | rel;
        for (size_t i = g_menuHist.size(); i-- > 0;)
        {
            const MenuChange &c = g_menuHist[i];
            const uint64_t k = (static_cast<uint64_t>(c.phase) << 32) | c.tick;
            if (k <= key)
                return c.phase == phase ? c.in : KzNetInput{};
        }
        return KzNetInput{};
    }

    // One menu read (pad 0, outside a game frame) in a session. local = this PC's pad for it. Never blocks: a pad read
    // that waits changes how the game runs (it read the pad more often per vblank in a test); all waiting happens in
    // menuVBlankGate, before a vblank starts.
    KzNetInput menuEntry(const KzNetInput &local)
    {
        const uint32_t now = g_vclock ? static_cast<uint32_t>(std::llround(g_vclock() * 60.0)) : 0u;
        bool newPhase = false;
        if (g_phase != g_frame)
        {
            g_phase = g_frame;
            g_phaseTick0 = now;
            newPhase = true;
        }
        const uint32_t rel = now - g_phaseTick0;
        ++g_menuSeq;
        std::lock_guard lock(g_sockMutex);
        if (g_mode == Mode::Host)
        {
            static KzNetInput last;
            if (newPhase || std::memcmp(last.b, local.b, 6) != 0)
            {
                g_menuHist.push_back({g_phase, rel, local});
                last = local;
            }
            // progress = first vblank not final yet: reads of earlier vblanks are all recorded
            const uint64_t key = (static_cast<uint64_t>(g_phase) << 32) | rel;
            if (key > g_hostProgress)
                g_hostProgress = key;
            sendMenu(false);
            return local; // the host's game sees its pad at once; what it saw is what the joiner replays
        }
        return menuInputAt(g_phase, rel);
    }

    // Before every vblank (EE executor, deterministic point). In a menu phase: pacing (KZ_MENU_PACE vblanks per second,
    // default 60; the session runs with PS2X_NO_PACING) and, on the joiner, waiting until the host's input for that
    // vblank is known. The host's socket is pumped here too (its handshake thread stops at the first game frame).
    void menuVBlankGate(uint64_t nextTick)
    {
        if (!sessionLive() || g_phase != g_frame)
            return;
        static const double hz = std::getenv("KZ_MENU_PACE") ? std::atof(std::getenv("KZ_MENU_PACE")) : static_cast<double>(kzTimingRate());
        if (hz > 0.0)
        {
            static double next = 0.0;
            const double t = nowSec();
            // Catch up after a short delay (v0.1.4 never caught up: with two windows on one PC movies stuttered more).
            if (next < t - 0.25)
                next = t;
            next += 1.0 / hz;
            while (nowSec() < next)
                std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        const uint32_t rel = static_cast<uint32_t>(nextTick) - g_phaseTick0;
        // Joiner: the host drives every menu (title, pause, mission failed, between levels): say so on screen.
        if (g_mode == Mode::Join)
        {
            const bool want = !g_ps2InMovie.load(std::memory_order_relaxed);
            if (want != g_menuBannerOn)
            {
                g_menuBannerOn = want;
                kzGsBanner(want ? "El jugador 1 controla los menus. Espera a que elija." : "");
            }
        }
        // KZ_NET_VBLOG=path: wall time of every menu/movie vblank (pacing analysis: stutter in menus and movies)
        static std::FILE *vblog = []() -> std::FILE * {
            const char *e = std::getenv("KZ_NET_VBLOG");
            return e && *e ? std::fopen(e, "w") : nullptr;
        }();
        struct VbLogEnd
        {
            uint32_t phase, rel;
            ~VbLogEnd()
            {
                if (vblog)
                    std::fprintf(vblog, "%u %u %.6f\n", phase, rel, nowSec());
            }
        } vbLogEnd{g_phase, rel};
        if (g_mode == Mode::Host)
        {
            std::lock_guard lock(g_sockMutex);
            // vblank rel starts: every earlier vblank of this phase is final
            const uint64_t key = (static_cast<uint64_t>(g_phase) << 32) | rel;
            if (key > g_hostProgress)
                g_hostProgress = key;
            if (g_handshakeStop.load() && !pump())
                endSession("the other player left");
            sendMenu(false);
            return;
        }
        // joiner: its reads during vblank rel use the host's reads up to rel: wait until the host has started rel + 1
        const uint64_t need = (static_cast<uint64_t>(g_phase) << 32) | rel;
        const double t0 = nowSec();
        struct WaitTag { WaitTag() { g_ps2EeWait.store("net: host menu input", std::memory_order_relaxed); } ~WaitTag() { g_ps2EeWait.store(nullptr, std::memory_order_relaxed); } } waitTag;
        for (;;)
        {
            {
                std::lock_guard lock(g_sockMutex);
                if (g_handshakeStop.load() && !pump())
                {
                    endSession("the other player left");
                    return;
                }
                if (g_hostProgress > need)
                    return;
                static const double bootWait = std::getenv("KZ_NET_WAIT") ? std::max(1.0, std::atof(std::getenv("KZ_NET_WAIT"))) : 300.0;
                // the host sends a keep-alive every 200 ms even while its game is busy (a level load can go 15+ s
                // without a vblank): give up only when nothing at all arrives
                if (nowSec() - std::max(t0, g_lastRecv.load()) > (g_phase == 0 && rel < 600 ? bootWait : g_timeout))
                {
                    endSession("no menu input from the host");
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    }
}

uint32_t kzNetPlayer2Char() { return g_p2Char; }
bool kzNetSessionMode() { return ((g_mode == Mode::Host || g_mode == Mode::Join) && !g_sessionOver) || (g_mode == Mode::Latch && g_p2scripted); }

bool kzNetPadProvider(int port, int slot, uint8_t *data, size_t size)
{
    if (port == 0 && slot == 0 && data && size >= 32)
    {
        ++g_pollsSinceFrame;
        ++g_pollTotal;
    }
    if (g_autostart < 0)
    {
        // Sessions: the host walks the menus itself (menu input stream); KZ_AUTOSTART=1 on the host for tests.
        const char *e = std::getenv("KZ_AUTOSTART");
        g_autostart = (e && (*e == '1' || *e == '2')) ? *e - '0' : 0;
        if (const char *v = std::getenv("KZ_AUTOSTART_EVERY"))
            g_autoEvery = std::max(1.0, std::atof(v));
    }
    // Session, outside a game frame: menus. Pad 0 = the host's pad on both PCs, pad 1 idle.
    const bool menuRead = (g_frame == 0 || g_pollsSinceFrame >= 2);
    if (sessionLive() && menuRead && data && size >= 32 && slot == 0 && port <= 1)
    {
        kzPadProvider(port, slot, data, size); // fills the fixed reply layout
        if (port == 1)
        {
            data[2] = data[3] = 0xFF;
            data[4] = data[5] = data[6] = data[7] = 0x80;
            return true;
        }
        KzNetInput local;
        std::memcpy(local.b, data + 2, 6);
        // KZ_AUTOSTART=2 (tests): also in the menus between levels / after a failed mission
        if (g_mode == Mode::Host && g_autostart && (g_frame == 0 || g_autostart == 2) && g_vclock)
        {
            const double t = g_vclock();
            // KZ_AUTOSTART_MOVIES=1 (tests): let movies play (no Cross while one is on screen)
            static const bool keepMovies = std::getenv("KZ_AUTOSTART_MOVIES") && *std::getenv("KZ_AUTOSTART_MOVIES") == '1';
            // KZ_AUTOSTART_UNTIL=<s> (tests): stop pressing after s seconds of vblank time (lets a level movie play)
            static const double until = std::getenv("KZ_AUTOSTART_UNTIL") ? std::atof(std::getenv("KZ_AUTOSTART_UNTIL")) : 1e30;
            const bool press = t > g_autoEvery && t < until && std::fmod(t, g_autoEvery) < 0.2 &&
                               !(keepMovies && g_ps2InMovie.load(std::memory_order_relaxed));
            local.b[0] = 0xFF;
            local.b[1] = press ? static_cast<uint8_t>(~(KZ_PAD_CROSS >> 8)) : 0xFF;
            local.b[2] = local.b[3] = local.b[4] = local.b[5] = 0x80;
        }
        const KzNetInput in = menuEntry(local);
        std::memcpy(data + 2, in.b, 6);
        if (g_pollLog)
            std::fprintf(g_pollLog, "m %u %.4f %02x%02x\n", g_menuSeq, g_vclock ? g_vclock() : 0.0, in.b[0], in.b[1]);
        if (g_pollLog && (g_menuSeq & 63u) == 0)
            std::fflush(g_pollLog);
        return true;
    }
    if (g_autostart && g_frame == 0 && data && size >= 32 && port == 0 && slot == 0 && g_vclock)
    {
        kzPadProvider(port, slot, data, size);
        const double t = g_vclock();
        static const bool keepMovies = std::getenv("KZ_AUTOSTART_MOVIES") && *std::getenv("KZ_AUTOSTART_MOVIES") == '1';
        const bool press = t > g_autoEvery && std::fmod(t, g_autoEvery) < 0.2 &&
                           !(keepMovies && g_ps2InMovie.load(std::memory_order_relaxed));
        data[2] = 0xFF;
        data[3] = press ? static_cast<uint8_t>(~(KZ_PAD_CROSS >> 8)) : 0xFF;
        data[4] = data[5] = data[6] = data[7] = 0x80;
        return true;
    }
    // local latch tests, KZ_AUTOSTART=2: Cross in the menus after a game frame too (end-of-mission, retry...)
    if (g_mode == Mode::Latch && g_autostart == 2 && menuRead && g_frame > 0 && data && size >= 32 && port == 0 && slot == 0 && g_vclock)
    {
        kzPadProvider(port, slot, data, size);
        const double t = g_vclock();
        const bool press = std::fmod(t, g_autoEvery) < 0.2;
        data[2] = 0xFF;
        data[3] = press ? static_cast<uint8_t>(~(KZ_PAD_CROSS >> 8)) : 0xFF;
        data[4] = data[5] = data[6] = data[7] = 0x80;
        return true;
    }
    if (!kzNetActive() || g_frame == 0 || !data || size < 32 || slot != 0 || port > 1)
        return kzPadProvider(port, slot, data, size);
    // The game's pad 0 is player 0's latched input on both PCs, pad 1 player 1's (the co-op partner, kz_coop).
    kzPadProvider(port, slot, data, size); // fills the fixed reply layout (port 1: an idle pad)
    std::memcpy(data + 2, g_latched[port].b, 6);
    return true;
}

void kzNetFrame(uint8_t *, uint32_t frame)
{
    // 11-05: the pre-boot banners (host "Jugador 2 conectado. Arrancando...", joiner "Recibiendo la partida...") were
    // never cleared and stayed over the gameplay: clear every banner at the first game frame.
    static bool firstFrame = true;
    if (g_menuBannerOn || firstFrame)
    {
        firstFrame = false;
        g_menuBannerOn = false;
        kzGsBanner("");
    }
    if (g_pollLog)
    {
        std::fprintf(g_pollLog, "%u %u %llu\n", frame, g_pollsSinceFrame, static_cast<unsigned long long>(g_pollTotal));
        std::fflush(g_pollLog);
    }
    g_pollsSinceFrame = 0;
    if (g_mode == Mode::Off)
        return;
    if (g_mode == Mode::Host)
    {
        // menu progress: every menu phase before this frame is over on the host
        std::lock_guard lock(g_sockMutex);
        const uint64_t p = (static_cast<uint64_t>(frame) << 32) - 1u;
        if (p > g_hostProgress)
            g_hostProgress = p;
    }
    g_frame = frame;
    if (g_mode == Mode::Latch)
    {
        g_latched[0] = sampleLocal();
        g_latched[1] = g_p2scripted ? p2ScriptInput(frame) : KzNetInput{};
        return;
    }
    if (frame == 1)
    {
        // Lobby: both PCs wait at the level start until the other one is connected (KZ_NET_WAIT seconds, default 300).
        // The menus run at their own pace on each PC, so the first one there must not start alone.
        if (!g_connected.load())
        {
            static const double wait = std::getenv("KZ_NET_WAIT") ? std::max(1.0, std::atof(std::getenv("KZ_NET_WAIT"))) : 300.0;
            std::cout << "[kz] netplay: waiting for the other player at the level start (" << wait << " s)" << std::endl;
            logf("level start: waiting for the other player");
            const double t0 = nowSec();
            while (!g_connected.load() && nowSec() - t0 < wait)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (g_connected.load())
            {
                logf("connected after waiting %.1f s at the level start", nowSec() - t0);
                std::cout << "[kz] netplay: connected, starting together" << std::endl;
            }
        }
        g_handshakeStop.store(true);
        if (g_handshake.joinable())
            g_handshake.join();
        if (!g_connected.load())
        {
            endSession("the other player was not connected when the level started");
            return;
        }
        // Frames 1..delay have no sampled input yet: both sides use neutral input for them.
        for (uint32_t f = 1; f <= g_delay; ++f)
            g_local.emplace(f, KzNetInput{});
        logf("level start, delay %u", g_delay);
        g_sessionStarted.store(true);
    }
    if (g_sessionOver)
    {
        // The other player left: this PC goes on with its own player, the other one gets neutral input.
        const int meLeft = kzNetLocalPlayer();
        g_latched[meLeft] = sampleLocal();
        g_latched[meLeft ^ 1] = KzNetInput{};
        return;
    }
    const int me = kzNetLocalPlayer();
    g_local.emplace(frame + g_delay, sampleLocal());
    sendInputs();
    // Wait for the peer's input of this frame.
    const double t0 = nowSec();
    double lastSend = t0;
    bool stalled = false;
    if (!g_remote.count(frame))
        g_ps2EeWait.store("net: peer input for this frame", std::memory_order_relaxed);
    while (!g_remote.count(frame))
    {
        if (!pump())
        {
            endSession("the other player left");
            return kzNetFrame(nullptr, frame);
        }
        if (g_remote.count(frame))
            break;
        const double t = nowSec();
        // after menus / a level load the other PC may arrive much later (slower PC, longer load): 2 minutes there
        const double limit = (frame == 1 || g_phase + 1u == frame) ? std::max(120.0, g_timeout) : g_timeout;
        if (t - std::max(t0, g_lastRecv.load()) > g_timeout || t - t0 > 600.0 || (t - t0 > limit && g_lastRecv.load() < t0))
        {
            endSession("timeout waiting for the other player");
            return kzNetFrame(nullptr, frame);
        }
        if (t - lastSend > 0.010)
        {
            sendInputs();
            lastSend = t;
        }
        if (!stalled && t - t0 > 0.100)
        {
            stalled = true;
            logf("frame %u: waiting for the peer", frame);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    g_ps2EeWait.store(nullptr, std::memory_order_relaxed);
    if (stalled)
        logf("frame %u: peer input arrived after %.0f ms", frame, (nowSec() - t0) * 1000.0);
    const KzNetInput mine = g_local.count(frame) ? g_local[frame] : KzNetInput{};
    const KzNetInput theirs = g_remote[frame];
    g_latched[me] = mine;
    g_latched[1 - me] = theirs;
    g_remote.erase(g_remote.begin(), g_remote.lower_bound(frame > 120 ? frame - 120 : 0));
}

namespace
{
    std::filesystem::path mcSlot1(const std::filesystem::path &root)
    {
        // MemoryCard.cpp: port 1's card is the sibling folder "<root>_slot1"
        return root.parent_path() / (root.filename().string() + "_slot1");
    }

    // Host: snapshot of both memory card folders (KZ_MC_ROOT) at start-up.
    void buildMcBlob()
    {
        const char *mc = std::getenv("KZ_MC_ROOT");
        if (!mc || !*mc)
        {
            std::cerr << "[kz] netplay: KZ_MC_ROOT not set, the memory card is not sent to player 2" << std::endl;
            return;
        }
        const std::filesystem::path root = std::filesystem::path(mc).lexically_normal();
        int files = 0;
        for (int card = 0; card < 2; ++card)
        {
            const std::filesystem::path dir = card == 0 ? root : mcSlot1(root);
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec))
                continue;
            for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            {
                if (!it->is_regular_file(ec))
                    continue;
                const std::string rel = std::filesystem::relative(it->path(), dir, ec).generic_string();
                std::FILE *f = std::fopen(it->path().string().c_str(), "rb");
                if (!f)
                    continue;
                std::vector<uint8_t> data;
                uint8_t tmp[4096];
                size_t r;
                while ((r = std::fread(tmp, 1, sizeof(tmp), f)) > 0)
                    data.insert(data.end(), tmp, tmp + r);
                std::fclose(f);
                g_mcBlob.push_back(static_cast<uint8_t>(card));
                g_mcBlob.push_back(static_cast<uint8_t>(rel.size()));
                g_mcBlob.push_back(static_cast<uint8_t>(rel.size() >> 8));
                g_mcBlob.insert(g_mcBlob.end(), rel.begin(), rel.end());
                put32(g_mcBlob, static_cast<uint32_t>(data.size()));
                g_mcBlob.insert(g_mcBlob.end(), data.begin(), data.end());
                ++files;
            }
        }
        logf("memory card snapshot: %d files, %zu bytes", files, g_mcBlob.size());
    }

    // Joiner, before its game boots: connect (HELLO until WELCOME), then fetch the host's memory card and write it over
    // the local KZ_MC_ROOT folders. Returns false if the host never answered (the session is then off: solo game).
    bool joinAtStartup()
    {
        const double wait = std::getenv("KZ_NET_WAIT") ? std::max(1.0, std::atof(std::getenv("KZ_NET_WAIT"))) : 300.0;
        std::cout << "[kz] netplay: connecting to the host (" << wait << " s)..." << std::endl;
        const double t0 = nowSec();
        while (!g_connected.load() && nowSec() - t0 < wait)
        {
            {
                const int left = static_cast<int>(std::max(0.0, wait - (nowSec() - t0)));
                char text[160];
                std::snprintf(text, sizeof(text), "KILLZONE CO-OP\n\nConectando con el anfitrion...  %d:%02d", left / 60, left % 60);
                kzGsBanner(text);
                kzGsIdlePresent();
            }
            std::vector<uint8_t> h = header(kHello);
            put32(h, kVersion);
            put32(h, g_p2Char);
            sendRaw(h);
            for (int i = 0; i < 20 && !g_connected.load(); ++i)
            {
                pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (!g_connected.load())
            return false;
        std::cout << "[kz] netplay: connected, receiving the host's memory card" << std::endl;
        kzGsBanner("KILLZONE CO-OP\n\nConectado. Recibiendo la partida del anfitrion...");
        kzGsIdlePresent();
        double last = nowSec();
        for (;;)
        {
            const uint32_t chunks = g_mcTotal ? (g_mcTotal + kMcChunk - 1) / kMcChunk : 1;
            uint32_t missing = 0, asked = 0;
            for (uint32_t i = 0; i < chunks; ++i)
                if (!g_mcChunks.count(i))
                {
                    ++missing;
                    if (asked < 16)
                    {
                        std::vector<uint8_t> r = header(kMcReq);
                        put32(r, i);
                        sendRaw(r);
                        ++asked;
                    }
                }
            if (g_mcTotal && !missing)
                break;
            if (g_mcTotal == 0 && !g_mcChunks.empty()) // an empty card: one empty chunk
                break;
            for (int i = 0; i < 10; ++i)
            {
                const size_t before = g_mcChunks.size();
                pump();
                if (g_mcChunks.size() != before)
                    last = nowSec();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (nowSec() - last > g_timeout)
            {
                std::cerr << "[kz] netplay: the host's memory card did not arrive" << std::endl;
                return false;
            }
        }
        std::vector<uint8_t> blob;
        for (auto &[i, c] : g_mcChunks)
            blob.insert(blob.end(), c.begin(), c.end());
        blob.resize(g_mcTotal);
        const char *mc = std::getenv("KZ_MC_ROOT");
        if (!mc || !*mc)
        {
            std::cerr << "[kz] netplay: KZ_MC_ROOT not set, the host's memory card is not used" << std::endl;
            return true;
        }
        const std::filesystem::path root = std::filesystem::path(mc).lexically_normal();
        std::error_code ec;
        for (const std::filesystem::path &d : {root, mcSlot1(root)})
        {
            std::filesystem::remove_all(d, ec);
            std::filesystem::create_directories(d, ec);
        }
        size_t p = 0;
        int files = 0;
        while (p + 7 <= blob.size())
        {
            const int card = blob[p];
            const size_t len = blob[p + 1] | (blob[p + 2] << 8);
            p += 3;
            if (p + len + 4 > blob.size())
                break;
            const std::string rel(reinterpret_cast<const char *>(blob.data() + p), len);
            p += len;
            const uint32_t sz = get32(blob.data() + p);
            p += 4;
            if (p + sz > blob.size() || rel.find("..") != std::string::npos)
                break;
            const std::filesystem::path out = (card == 0 ? root : mcSlot1(root)) / std::filesystem::path(rel);
            std::filesystem::create_directories(out.parent_path(), ec);
            if (std::FILE *f = std::fopen(out.string().c_str(), "wb"))
            {
                std::fwrite(blob.data() + p, 1, sz, f);
                std::fclose(f);
                ++files;
            }
            p += sz;
        }
        std::cout << "[kz] netplay: host's memory card written (" << files << " files, " << blob.size() << " bytes)" << std::endl;
        logf("memory card from host: %d files, %zu bytes", files, blob.size());
        return true;
    }
}

void kzNetInstall(PS2Runtime &)
{
    if (const char *e = std::getenv("KZ_NET_LOG"); e && *e)
        g_log = std::fopen(e, "w");
    if (const char *e = std::getenv("KZ_NET_POLLLOG"); e && *e)
        g_pollLog = std::fopen(e, "w");
    if (const char *e = std::getenv("KZ_NET_DELAY"))
        g_delay = static_cast<uint32_t>(std::max(1, std::min(30, std::atoi(e))));
    if (const char *e = std::getenv("KZ_NET_DIGEST"))
        g_digestEvery = static_cast<uint32_t>(std::max(0, std::atoi(e)));
    // KZ_NET_FPS (host): session frame rate, 30 (console pace, default) or 40..120
    if (const char *e = std::getenv("KZ_NET_FPS"); e && *e)
        g_sessionFps = static_cast<uint32_t>(std::clamp(std::atoi(e), 30, 120));
    if (const char *e = std::getenv("KZ_NET_TIMEOUT"))
        g_timeout = std::max(1.0, std::atof(e));
    if (const char *e = std::getenv("KZ_NET_P2SCRIPT"); e && *e)
        parseP2Script(e);
    // KZ_COOP_CHAR (joiner; latch tests): player 2's character, Templar | Luger | Rico | Hakha (or 0..3)
    if (const char *e = std::getenv("KZ_COOP_CHAR"); e && *e)
    {
        const std::string c(e);
        if (c == "1" || _stricmp(e, "luger") == 0)
            g_p2Char = 1;
        else if (c == "2" || _stricmp(e, "rico") == 0 || _stricmp(e, "choyan") == 0)
            g_p2Char = 2;
        else if (c == "3" || _stricmp(e, "hakha") == 0)
            g_p2Char = 3;
    }
    const char *net = std::getenv("KZ_NET");
    if (net && _stricmp(net, "host") == 0)
        g_mode = Mode::Host;
    else if (net && _stricmp(net, "join") == 0)
        g_mode = Mode::Join;
    else if ((std::getenv("KZ_NET_LATCH") && *std::getenv("KZ_NET_LATCH") == '1') || g_p2scripted)
        g_mode = Mode::Latch;
    if (g_mode == Mode::Off)
        return;
    if (g_mode == Mode::Latch)
    {
        std::cout << "[kz] netplay: local latch test" << (g_p2scripted ? " with scripted player 2" : "") << std::endl;
        return;
    }
    uint16_t port = 47474;
    if (const char *e = std::getenv("KZ_NET_PORT"))
        port = static_cast<uint16_t>(std::atoi(e));
    if (g_mode == Mode::Host)
    {
        if (!openSocket(port))
        {
            g_mode = Mode::Off;
            return;
        }
        std::cout << "[kz] netplay: hosting on UDP port " << port << ", waiting for player 2" << std::endl;
        logf("host on port %u, delay %u", port, g_delay);
        buildMcBlob();
    }
    else
    {
        std::string addr = std::getenv("KZ_NET_ADDR") ? std::getenv("KZ_NET_ADDR") : "127.0.0.1";
        uint16_t hostPort = port;
        if (size_t c = addr.find(':'); c != std::string::npos)
        {
            hostPort = static_cast<uint16_t>(std::atoi(addr.c_str() + c + 1));
            addr.resize(c);
        }
        if (!openSocket(0))
        {
            g_mode = Mode::Off;
            return;
        }
        g_peer.sin_family = AF_INET;
        g_peer.sin_port = htons(hostPort);
        if (inet_pton(AF_INET, addr.c_str(), &g_peer.sin_addr) != 1)
        {
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            if (getaddrinfo(addr.c_str(), nullptr, &hints, &res) == 0 && res)
            {
                g_peer.sin_addr = reinterpret_cast<sockaddr_in *>(res->ai_addr)->sin_addr;
                freeaddrinfo(res);
            }
        }
        g_havePeer = true;
        std::cout << "[kz] netplay: joining " << addr << ":" << hostPort << std::endl;
        logf("join %s:%u", addr.c_str(), hostPort);
        if (!joinAtStartup())
        {
            std::cerr << "[kz] netplay: could not join the host, playing alone" << std::endl;
            g_mode = Mode::Off;
            return;
        }
    }
    // Session frame rate: the host's choice (the joiner got it in WELCOME), applied before the game boots on both PCs.
    {
        const uint32_t fps = g_sessionFps;
        const int rate = fps <= 30 ? 60 : static_cast<int>(fps);
        kzTimingInit(rate, rate);
        kzLockstepSetStep(fps <= 30 ? 2u : 1u);
        g_paceHz = fps <= 30 ? 30.0 : static_cast<double>(fps);
        // The game does a 30 fps console frame of work per game frame: above 30 the EE clock is scaled (60 fps: 0.5) so
        // a frame's charged cycles fit its vblank period. Otherwise emulated time ran ~1.08x at 60 (the SPU2 made too
        // much sound and the output dropped pieces). KZ_NET_EE_SCALE=<f> overrides.
        const double scale = std::getenv("KZ_NET_EE_SCALE") ? std::atof(std::getenv("KZ_NET_EE_SCALE")) : (fps <= 30 ? 1.0 : 30.0 / fps);
        ps2SetEeCycleScale(scale);
        logf("session frame rate %u (vblank %d Hz, %u vblanks per game frame)", fps, rate, kzLockstepStep());
        std::cout << "[kz] netplay: session frame rate " << fps << " fps" << std::endl;
    }
    // The wait for the peer's input sleeps in 0.5 ms steps: without a 1 ms timer resolution Windows rounds each sleep up
    // to ~15.6 ms and every frame that waits even a little loses a whole timer tick (a paced session ran at ~4 fps).
    timeBeginPeriod(1);
    g_handshake = std::thread(handshakeMain);
    g_ps2VBlankGate = &menuVBlankGate;
    g_keepAlive = std::thread(keepAliveMain);
    g_keepAlive.detach();
    if (g_mode == Mode::Host)
    {
        // Wait for the joiner before the game boots: both games then run the same menus (host's input) from the same
        // memory card (sent to the joiner while it connects).
        const double wait = std::getenv("KZ_NET_WAIT") ? std::max(1.0, std::atof(std::getenv("KZ_NET_WAIT"))) : 300.0;
        std::cout << "[kz] netplay: waiting for player 2 (" << wait << " s) before the game starts" << std::endl;
        const double t0 = nowSec();
        // on-screen notice with the countdown (the game window would stay black until player 2 joins)
        double lastBanner = -1.0;
        while (!g_connected.load() && nowSec() - t0 < wait)
        {
            const double now = nowSec();
            if (now - lastBanner >= 0.1)
            {
                lastBanner = now;
                const int left = static_cast<int>(std::max(0.0, wait - (now - t0)));
                char text[200];
                std::snprintf(text, sizeof(text), "KILLZONE CO-OP\n\nEsperando al jugador 2...  %d:%02d\nPasale tu IP (puerto UDP %u). Si no llega, jugas solo.",
                              left / 60, left % 60, static_cast<unsigned>(port));
                kzGsBanner(text);
                kzGsIdlePresent();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        kzGsBanner(g_connected.load() ? "Jugador 2 conectado. Arrancando..." : "");
        kzGsIdlePresent();
        if (!g_connected.load())
        {
            std::lock_guard lock(g_sockMutex);
            endSession("player 2 did not join");
        }
        else
            std::cout << "[kz] netplay: player 2 is here, the host drives the menus" << std::endl;

    }
}

int kzNetSessionVblankRate()
{
    if (g_mode != Mode::Host && g_mode != Mode::Join)
        return 60;
    return g_sessionFps <= 30 ? 60 : static_cast<int>(g_sessionFps);
}

void kzFramePace()
{
    static const double envHz = []() {
        const char *e = std::getenv("KZ_FRAME_PACE");
        return e ? std::atof(e) : 0.0;
    }();
    const double hz = g_paceHz >= 0.0 ? g_paceHz : envHz;
    if (hz <= 0.0)
        return;
    using clock = std::chrono::steady_clock;
    static bool init = false;
    static clock::time_point base;
    static uint64_t n = 0;
    if (!init)
    {
        timeBeginPeriod(1);
        init = true;
        base = clock::now();
        n = 0;
        return;
    }
    ++n;
    const auto period = std::chrono::duration<double>(1.0 / hz);
    auto target = base + std::chrono::duration_cast<clock::duration>(period * static_cast<double>(n));
    auto now = clock::now();
    if (now > target + std::chrono::milliseconds(250)) // far behind (slow host, loading): do not race to catch up
    {
        base = now;
        n = 0;
        return;
    }
    if (target - now > std::chrono::milliseconds(2))
        std::this_thread::sleep_until(target - std::chrono::milliseconds(1));
    while (clock::now() < target)
        std::this_thread::yield();
}
