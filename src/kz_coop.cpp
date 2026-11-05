// Fork (KZ1 co-op): player 2 in the campaign. See kz_coop.h.
//
// Lua access. The levels are driven by Lua 4.0 scripts; their global table already has what co-op needs
// (AddPlayer/CreatePlayer, AddLuger, AddHumanoidAIActor, gGamePlayInfo, gGameInterface...). The interpreter keeps its
// parser, so source text can be run with lua_dobuffer (ES FUN_002e7990, called by the `dostring' builtin FUN_002eff60).
// The level script's lua_State is at [[0x5DB110] + 0x1C] (script object, vtable 0x529260; ES, level 1, found from the
// argument of luaL_check_lstr FUN_002ef068 during the level start).
//
// Running Lua from the frame limiter wrapper: the guest is redirected into lua_dobuffer with its return address set to
// the frame limiter itself; when Lua returns (possibly after other guest threads ran meanwhile) the limiter is entered
// again, the wrapper restores the registers it saved and runs the real limiter. The code text lives on the guest stack
// below the limiter's frame for the duration of the call.
//
//   KZ_LUA="<frame>:<code>;;<frame>:<code>"  run code at those game frames (tests). The global KZOUT, if the code sets
//                                           it to a string, is printed after the call ("[kz] lua: ...").
#include "kz_coop.h"

#include "kz_addrs.h"
#include "kz_netplay.h"
#include "kz_region.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

extern std::atomic<uint32_t> g_ps2GifSubmits; // ps2_memory.cpp (fork)
extern std::atomic<bool> g_ps2GsMute;
extern std::atomic<int> g_kzShotNow; // kz_gs.cpp
extern std::atomic<int> g_ps2KickProbe;
extern std::atomic<uint32_t> g_ps2GsMuteList[2][2];
extern std::atomic<bool> g_ps2MuteSkip;
extern std::atomic<uint32_t> g_ps2ViewStarts;

namespace
{
    constexpr uint32_t kRamMask = 0x01FFFFFFu;
    uint32_t rd32(const uint8_t *ram, uint32_t a)
    {
        uint32_t v;
        std::memcpy(&v, ram + (a & kRamMask), 4);
        return v;
    }
    void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & kRamMask), &v, 4); }
    uint32_t reg(const R5900Context *ctx, int r) { return getRegU32(ctx, r); }
    void setReg(R5900Context *ctx, int r, uint32_t v) { ctx->r[r] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(v))); }

    struct LuaAddrs
    {
        uint32_t doBuffer = 0;     // lua_dobuffer(L, buff, size, name)
        uint32_t scriptObj = 0;    // global holding the level script object
        uint32_t scriptVtable = 0; // its vtable (sanity check)
    };
    LuaAddrs luaAddrs()
    {
        LuaAddrs a;
        if (kzIsEs())
        {
            a.doBuffer = 0x002E7990u;
            a.scriptObj = 0x005DB110u;
            a.scriptVtable = 0x00529260u;
        }
        return a;
    }

    uint32_t luaState(const uint8_t *ram)
    {
        const LuaAddrs a = luaAddrs();
        if (!a.scriptObj)
            return 0;
        const uint32_t obj = rd32(ram, a.scriptObj);
        if (obj < 0x100000u || obj >= 0x02000000u || rd32(ram, obj) != a.scriptVtable)
            return 0;
        const uint32_t L = rd32(ram, obj + 0x1C);
        return L >= 0x100000u && L < 0x02000000u ? L : 0;
    }

    // Lua 4.0 global table: L+0x44 Hash* (node array at +0, size at +8); Node = key TObject (tag, value), val TObject,
    // next (20 bytes); tag 3 = string, TString text at +0x14.
    std::string cstr(const uint8_t *ram, uint32_t a, size_t max = 512)
    {
        std::string s;
        for (size_t i = 0; i < max; ++i)
        {
            const char c = static_cast<char>(ram[(a + i) & kRamMask]);
            if (!c)
                break;
            s.push_back(c);
        }
        return s;
    }
    bool luaGlobal(const uint8_t *ram, uint32_t L, const char *name, uint32_t &tag, uint32_t &value)
    {
        const uint32_t h = rd32(ram, L + 0x44);
        if (h < 0x100000u || h >= 0x02000000u)
            return false;
        const uint32_t node = rd32(ram, h), size = rd32(ram, h + 8);
        if (node < 0x100000u || size > 65536u)
            return false;
        for (uint32_t i = 0; i < size; ++i)
        {
            const uint32_t n = node + i * 20;
            if (rd32(ram, n) != 3u)
                continue;
            if (cstr(ram, rd32(ram, n + 4) + 0x14, 64) == name)
            {
                tag = rd32(ram, n + 8);
                value = rd32(ram, n + 12);
                return true;
            }
        }
        return false;
    }

    std::deque<std::string> g_luaQueue;
    std::vector<std::pair<uint32_t, std::string>> g_luaScript; // KZ_LUA
    PS2Runtime::RecompiledFunction g_doBuffer = nullptr;
    struct Saved
    {
        bool active = false;
        bool quiet = false; // code starting with "--q": only logged when it sets KZOUT
        uint32_t a[4], ra, sp, pc;
    } g_saved;
    uint32_t g_frameNow = 0;
    uint32_t g_moveAt = 0;      // frame at which player 2 is put next to player 1 (after a respawn)
    bool g_moveByActor = false; // locate player 2 through its slot's actor (respawn) instead of the closest pair
    bool g_moveP1 = false;      // move player 1 next to player 2 instead (player 1 respawned)
    bool g_p1RespawnPending = false;
    uint32_t g_hiddenCtlSlot = 0, g_hiddenCtl = 0; // player 2's controller hidden during a checkpoint save/restore

    // Starts a Lua call; the guest returns into the frame limiter when it is done.
    bool startLua(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, const std::string &code)
    {
        const uint32_t L = luaState(rdram);
        if (!L || !g_doBuffer)
        {
            std::cerr << "[kz] lua: no level script state, code dropped" << std::endl;
            return false;
        }
        const uint32_t sp = reg(ctx, 29);
        const uint32_t len = static_cast<uint32_t>(code.size());
        const uint32_t area = (len + 16u + 15u) & ~15u;
        const uint32_t buf = sp - area - 0x40u;
        const uint32_t name = buf + ((len + 4u) & ~3u);
        std::memcpy(rdram + (buf & kRamMask), code.data(), len);
        rdram[(buf + len) & kRamMask] = 0;
        std::memcpy(rdram + (name & kRamMask), "=kz", 4);
        g_saved.active = true;
        g_saved.quiet = code.rfind("--q", 0) == 0;
        for (int i = 0; i < 4; ++i)
            g_saved.a[i] = reg(ctx, 4 + i);
        g_saved.ra = reg(ctx, 31);
        g_saved.sp = sp;
        g_saved.pc = ctx->pc;
        setReg(ctx, 29, (buf - 0x100u) & ~15u);
        setReg(ctx, 4, L);
        setReg(ctx, 5, buf);
        setReg(ctx, 6, len);
        setReg(ctx, 7, name);
        setReg(ctx, 31, kzAddrs().frameLimiter);
        ctx->pc = luaAddrs().doBuffer;
        g_doBuffer(rdram, ctx, rt);
        return true;
    }

    void parseScript()
    {
        const char *e = std::getenv("KZ_LUA");
        if (!e || !*e)
            return;
        std::string all(e);
        size_t pos = 0;
        while (pos < all.size())
        {
            size_t end = all.find(";;", pos);
            std::string it = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? all.size() : end + 2;
            const size_t c = it.find(':');
            if (c == std::string::npos)
                continue;
            g_luaScript.emplace_back(static_cast<uint32_t>(std::atoi(it.c_str())), it.substr(c + 1));
        }
    }
}

void kzLuaQueue(const std::string &code) { g_luaQueue.push_back(code); }

bool kzCoopResume(uint8_t *rdram, R5900Context *ctx)
{
    if (!g_saved.active || ctx->pc != kzAddrs().frameLimiter)
        return false;
    g_saved.active = false;
    for (int i = 0; i < 4; ++i)
        setReg(ctx, 4 + i, g_saved.a[i]);
    setReg(ctx, 31, g_saved.ra);
    setReg(ctx, 29, g_saved.sp);
    const int result = static_cast<int>(reg(ctx, 2));
    uint32_t tag = 0, value = 0;
    const uint32_t L = luaState(rdram);
    std::string out;
    if (L && luaGlobal(rdram, L, "KZOUT", tag, value) && tag == 3u)
        out = cstr(rdram, value + 0x14, 4000);
    if (!g_saved.quiet || !out.empty() || result != 0)
        std::cout << "[kz] lua: done (result " << result << ")" << (out.empty() ? "" : " KZOUT=") << out << std::endl;
    // Player 2 came back (respawn): a few frames later it is put next to player 1 (same frame on every PC).
    if (out.rfind("p2: respawned", 0) == 0)
    {
        g_moveAt = g_frameNow + 3;
        g_moveByActor = true;
    }
    if (out.rfind("p1: respawn now", 0) == 0)
        g_p1RespawnPending = true;
    if (out.rfind("p1: respawned", 0) == 0) // player 1 back: next to player 2
    {
        g_moveAt = g_frameNow + 3;
        g_moveByActor = true;
        g_moveP1 = true;
    }
    return true;
}

// Player slots: [[gameObject]+0x74]+0x220, 0x1E0 bytes each; +0x80 the profile (controller map, settings), +0x84 the
// joystick index (-1 = none). A slot added from Lua has no profile, and the joystick controller update
// (ctrlUpdate, ES FUN_0023f2b8) returns at once without one: player 2 shares player 1's profile.
uint32_t kzCoopSlot(const uint8_t *ram, int i)
{
    const uint32_t g = rd32(ram, kzAddrs().gameObjectPtr);
    if (g < 0x100000u || g >= 0x02000000u)
        return 0;
    const uint32_t p = rd32(ram, g + 0x74);
    if (p < 0x100000u || p >= 0x02000000u)
        return 0;
    if (static_cast<int>(rd32(ram, p + 0x218)) <= i) // slot count
        return 0;
    const uint32_t s = rd32(ram, p + 0x220);
    return s >= 0x100000u && s < 0x02000000u ? s + static_cast<uint32_t>(i) * 0x1E0u : 0;
}

namespace
{
    // Lua helpers of the co-op code (run in the level script state). KZFIX(a, b): the character globals that refer to
    // actor a refer to b afterwards. kSetup: KZP1/KZP2 = the two players' actors, character globals on player 1.
    const std::string kFixDef =
        "function KZFIX(a, b)\n"
        "  if a and b then\n"
        "    for i, n in {'PLAYER', 'TEMPLAR', 'LUGER', 'CHOYAN', 'HAKHA'} do\n"
        "      if getglobal(n) == a then setglobal(n, b) end\n"
        "    end\n"
        "  end\n"
        "end\n"
        "function KZADDP2(s, i)\n"
        "  local p = PLAYER\n"
        "  AddPlayer(s, i)\n"
        "  local a = PLAYER\n"
        "  PLAYER = p\n"
        "  KZFIX(a, KZP1)\n"
        "  return a\n"
        "end\n";
    const std::string kSetup =
        "KZP1 = gGamePlayInfo:GetPlayerActor(0)\n"
        "KZP2 = gGamePlayInfo:GetPlayerActor(1)\n"
        "KZFIX(KZP2, KZP1)\n"
        "KZDEAD = 0\n";
}

void kzCoopEnsureTwo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt); // below (p2 configuration)
// Level change (setLevelWrap): one player while the new level loads; its start checkpoint save (cpSaveWrap) arms
// g_addP2At = that frame + 30, when two players are configured again and player 2 is added (as at frame 30 of the
// first level). Game frames count across levels.
bool g_levelChangePending = false;
uint32_t g_addP2At = 0;
// 11-05 "native player 2" (default; KZ_COOP_LEGACY=1 = the older patches): player 2 is configured like a Battlefields
// split-screen player before the level loads (ControlType 'Human', so the level's Load1pBase loads its model in
// OnLoad) and created by the level library's own AddPlayer(spawn, 1) right after the level's OnInitialize, before the
// level-start checkpoint (engine order FUN_001becc0: OnIntro, OnLoad, per-slot setup, OnInitialize, checkpoint). No
// detach at level change (the unload FUN_001bdcf8 empties every slot itself), no hidden controller at checkpoints.
// See Port/reportes/kz1-analisis-multijugador.md.
bool kzCoopNative()
{
    static const bool legacy = std::getenv("KZ_COOP_LEGACY") != nullptr;
    return !legacy;
}

bool kzCoopFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t frame)
{
    // back to two players if something (a checkpoint restore that yielded) left the game with one
    if (frame > 30 && frame % 30 == 10 && kzNetHasPlayer2() && !g_levelChangePending && !g_addP2At)
        kzCoopEnsureTwo(rdram, ctx, rt);
    if (g_addP2At && frame >= g_addP2At && kzNetHasPlayer2())
        kzCoopEnsureTwo(rdram, ctx, rt);
    if (const uint32_t s0 = kzCoopSlot(rdram, 0), s1 = kzCoopSlot(rdram, 1); s0 && s1)
    {
        if (rd32(rdram, s1 + 0x84) == 1u && rd32(rdram, s1 + 0x80) == 0u && rd32(rdram, s0 + 0x80) != 0u)
        {
            // The profile is reference counted (+4, FUN_00178430 releases) and the slot destructor (FUN_001b85c8)
            // releases +0x80: without its own reference a level restart / checkpoint load released it twice and broke
            // the heap (both games hung in malloc, black screen).
            const uint32_t prof = rd32(rdram, s0 + 0x80);
            wr32(rdram, s1 + 0x80, prof);
            if (prof >= 0x100000u && prof < 0x02000000u)
                wr32(rdram, prof + 4, rd32(rdram, prof + 4) + 1);
            std::cout << "[kz] coop: player 2 slot uses player 1's profile" << std::endl;
        }
        // With two players the game splits the screen (slot 0 top half, slot 1 bottom half, viewport +0xF0 x, +0xF4 y,
        // +0xF8 right, +0xFC bottom). Each PC shows one player full screen, and the bottom-half view also drove the
        // PCSX2 hardware GS into bogus render target sizes (crash in GSTextureCache::Target::Update, level 1 frame 219).
        // Both slots get the full screen, the same on every PC (the simulation stays identical).
        const uint32_t right = std::max(rd32(rdram, s0 + 0xF8), rd32(rdram, s1 + 0xF8));
        const uint32_t bottom = std::max(rd32(rdram, s0 + 0xFC), rd32(rdram, s1 + 0xFC));
        for (const uint32_t s : {s0, s1})
        {
            wr32(rdram, s + 0xF0, 0);
            wr32(rdram, s + 0xF4, 0);
            wr32(rdram, s + 0xF8, right);
            wr32(rdram, s + 0xFC, bottom);
        }
    }
    // Player 2 is created on player 1's spawn point (the level has one player spawn; other spawn names crash CreatePlayer
    // or are far away). A few frames later, on every PC at the same frame, move player 2's character controller
    // KZ_COOP_P2_OFFSET metres (default 1.5) to the side of player 1 so they do not stand inside each other.
    g_frameNow = frame;
    {
        static std::vector<uint32_t> shotFrames = []() {
            std::vector<uint32_t> v;
            if (const char *e = std::getenv("KZ_SHOT_FRAMES"))
                for (const char *p = e; *p;)
                {
                    v.push_back(static_cast<uint32_t>(std::strtoul(p, const_cast<char **>(&p), 10)));
                    while (*p == ',' || *p == ' ')
                        ++p;
                }
            return v;
        }();
        for (const uint32_t f : shotFrames)
            if (f == frame)
                g_kzShotNow.store(1);
    }
    if (g_hiddenCtlSlot)
    {
        if (rd32(rdram, g_hiddenCtlSlot + 0xD8) == 0)
            wr32(rdram, g_hiddenCtlSlot + 0xD8, g_hiddenCtl);
        g_hiddenCtlSlot = g_hiddenCtl = 0;
    }
    // Player 1 respawn, step 2. AddPlayer gives slot 0 a new actor and releases the old one, but level scripts keep
    // references to the old player actor (a Lua getter, FUN_002a6110, then called a virtual method of the freed object:
    // jump to 0x2c130200). One extra reference (+4, the game's reference count) keeps the dead body valid.
    if (g_p1RespawnPending)
    {
        g_p1RespawnPending = false;
        if (const uint32_t s0 = kzCoopSlot(rdram, 0))
            if (const uint32_t old = rd32(rdram, s0 + 0xD4); old >= 0x100000u && old < 0x02000000u)
                wr32(rdram, old + 4, rd32(rdram, old + 4) + 1);
        g_luaQueue.push_front(
            "KZOLD = KZP1\n"
            "call(function() AddPlayer(KZSPAWN or 'Spawn_Player', 0) end, {}, 'x', function(e) KZOUT = 'p1: respawn ERROR ' .. tostring(e) end)\n"
            "KZP1 = gGamePlayInfo:GetPlayerActor(0)\n"
            "if KZP1 and KZP1 ~= KZOLD then PLAYER = KZP1 KZFIX(KZOLD, KZP1) KZFIX(KZP2, KZP1) KZOUT = 'p1: respawned' end\n");
    }
    // On by default (KZ_COOP_P2_OFFSET=0 disables): verified that the actor and its camera follow the controller.
    static const bool moveOn = !(std::getenv("KZ_COOP_P2_OFFSET") && std::atof(std::getenv("KZ_COOP_P2_OFFSET")) == 0.0);
    if (frame == 31 && moveOn)
    {
        g_moveAt = 34;
        g_moveByActor = false;
    }
    if (frame == g_moveAt && moveOn)
    {
        g_moveAt = 0;
        struct ResetP1 { ~ResetP1() { g_moveP1 = false; } } resetP1;
        static const float off = std::getenv("KZ_COOP_P2_OFFSET") ? static_cast<float>(std::atof(std::getenv("KZ_COOP_P2_OFFSET"))) : 1.5f;
        // (default 1.5 m)
        const uint32_t ctlVt = kzAddrs().ctlVtable;
        std::vector<uint32_t> ctl;
        for (uint32_t a = 0x100000; a + 0x80 < 0x02000000u; a += 4)
            if (rd32(rdram, a) == ctlVt)
                ctl.push_back(a);
        uint32_t p1 = 0, p2 = 0;
        float best = 1e9f;
        if (g_moveByActor)
        {
            // After a respawn player 2 is at the level's player spawn, maybe far away: each player's character controller
            // is the one that refers to the actor of its slot (+0xD4).
            const uint32_t s0 = kzCoopSlot(rdram, 0), s1 = kzCoopSlot(rdram, 1);
            const uint32_t a0 = s0 ? rd32(rdram, s0 + 0xD4) : 0, a1 = s1 ? rd32(rdram, s1 + 0xD4) : 0;
            for (const uint32_t c : ctl)
                for (uint32_t o = 4; o < 0x400; o += 4)
                {
                    const uint32_t w = rd32(rdram, c + o);
                    if (a0 && w == a0 && !p1)
                        p1 = c;
                    if (a1 && w == a1 && !p2)
                        p2 = c;
                }
            best = p1 && p2 && p1 != p2 ? 0.0f : 1e9f;
            std::cout << "[kz] coop: respawn move, ctl p1 " << std::hex << p1 << " p2 " << p2 << std::dec << std::endl;
        }
        else
            for (size_t i = 0; i < ctl.size(); ++i)
                for (size_t j = i + 1; j < ctl.size(); ++j)
                {
                    float a[3], b[3];
                    std::memcpy(a, rdram + ctl[i] + 0x50, 12);
                    std::memcpy(b, rdram + ctl[j] + 0x50, 12);
                    const float d = std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]);
                    if (d < best)
                    {
                        best = d;
                        p1 = ctl[i];
                        p2 = ctl[j]; // the one created last (higher address)
                    }
                }
        if (p1 && p2 && best < 0.3f && kzNetHasPlayer2())
        {
            const uint32_t from = g_moveP1 ? p2 : p1, to = g_moveP1 ? p1 : p2;
            float p[3];
            std::memcpy(p, rdram + from + 0x50, 12);
            p[0] += off;
            std::memcpy(rdram + to + 0x50, p, 12);
            std::cout << "[kz] coop: player " << (g_moveP1 ? 1 : 2) << " moved " << off << " m from player " << (g_moveP1 ? 2 : 1)
                      << " (ctl " << std::hex << to << std::dec << ")" << std::endl;
        }
    }
    for (auto &[f, code] : g_luaScript)
        if (f == frame)
            g_luaQueue.push_back(code);
    // Player 2 joins at game frame 30 of a level when a second player takes part (netplay session or local test), on
    // every PC at the same frame. Same character as player 1 (its model is always loaded; other protagonists need their
    // resources cached by the level, see the report). gGamePlayInfo keeps two players for the next levels and checkpoint
    // reloads, where the level's own AddPlayers then creates player 2.
    //
    // AddPlayer also stores the new actor in the character's global (TEMPLAR = player 2 when both are Templar). The
    // level's end check (EndIfPlayerOrBuddyDied: PLAYER, TEMPLAR, LUGER, CHOYAN, HAKHA must be alive) would then fail the
    // mission when player 2 dies or leaves, so KZFIX points those globals back at player 1.
    static const bool noAuto = std::getenv("KZ_COOP_NOAUTO") != nullptr;
    const bool addNow = frame == 30 || (g_addP2At && frame >= g_addP2At);
    if (addNow && g_addP2At && frame >= g_addP2At)
    {
        g_addP2At = 0;
        std::cout << "[kz] coop: new level started, adding player 2" << std::endl;
    }
    if (addNow && kzNetHasPlayer2() && !noAuto)
        g_luaQueue.push_back(
            "KZOUT='p2: already there'\n" + kFixDef +
            "if gGamePlayInfo:GetNrOfPlayers() < 2 then\n"
            "  call(function()\n"
            "    gGamePlayInfo:SetNrOfPlayers(2)\n"
            "    gGamePlayInfo:SetPlayerCharacter(1, gGamePlayInfo:GetPlayerCharacter(0))\n"
            "    gGamePlayInfo:SetPlayerJoystick(1, 1)\n"
            "    KZP1 = PLAYER\n"
            "    AddPlayer(KZSPAWN or 'Spawn_Player', 1)\n"
            "    PLAYER = KZP1\n"
            "    KZOUT = 'p2: added'\n"
            "  end, {}, 'x', function(e) KZOUT = 'p2: ERROR ' .. tostring(e) end)\n"
            // configured before the load (configureP2): the slot exists, its actor not yet
            "elseif gGamePlayInfo:GetPlayerActor(1) == nil then\n"
            "  call(function()\n"
            "    KZP1 = PLAYER\n"
            "    AddPlayer(KZSPAWN or 'Spawn_Player', 1)\n"
            "    PLAYER = KZP1\n"
            "    KZOUT = 'p2: added as ' .. tostring(gGamePlayInfo:GetPlayerCharacter(1))\n"
            "  end, {}, 'x', function(e) KZOUT = 'p2: ERROR ' .. tostring(e) end)\n"
            "end\n" + kSetup);
    // Player 2 death (point 4), the Battlefields way (game mode script RespawnPlayer -> gGamePlayInfo:Respawn, which runs
    // the player's creation function, AddPlayer, again): checked once a second (30 game frames) on every PC; after
    // KZ_COOP_RESPAWN seconds (default 5) dead, player 2 comes back and is put next to player 1.
    static const int respawnSec = std::getenv("KZ_COOP_RESPAWN") ? std::max(1, std::atoi(std::getenv("KZ_COOP_RESPAWN"))) : 5;
    static const bool noRespawn = std::getenv("KZ_COOP_NORESPAWN") != nullptr;
    static bool left = false;
    if (frame > 60 && frame % 30 == 15 && kzNetHasPlayer2() && !left && !noAuto && !noRespawn)
        g_luaQueue.push_back(
            "--q\nKZOUT = nil\n"
            "call(function()\n" // errors logged (watchdog ERROR) instead of an anonymous result 1
            // a new level has a new script state: set the helpers up again (the level's AddPlayers made player 2)
            "if not KZFIX and gGamePlayInfo:GetNrOfPlayers() >= 2 then\n" + kFixDef +
            "  KZOUT = 'p2: globals set for this level'\n"
            "  KZP1 = gGamePlayInfo:GetPlayerActor(0)\n"
            "  if gGamePlayInfo:GetPlayerActor(1) == nil and KZP1 then\n"
            "    KZADDP2(KZSPAWN or 'Spawn_Player', 1)\n"
            "    KZOUT = 'p2: respawned at the level start as ' .. tostring(gGamePlayInfo:GetPlayerCharacter(1))\n"
            "  end\n" + kSetup +
            "end\n"
            "if gGamePlayInfo:GetNrOfPlayers() >= 2 then\n"
            // Player 1 (host) death: with two players configured the game's own end-of-mission check does not fire (the
            // camera just circles the body). Co-op rule: while player 2 lives, player 1 comes back after the same delay
            // next to player 2; both dead = mission failed (the game's lost status -> Retry screen).
            "  local a0 = gGamePlayInfo:GetPlayerActor(0)\n"
            "  local a1 = gGamePlayInfo:GetPlayerActor(1)\n"
            "  local d0 = a0 == nil or not a0:IsAlive()\n"
            "  local d1 = a1 == nil or not a1:IsAlive()\n"
            "  if d0 and d1 then\n"
            "    KZBOTH = (KZBOTH or 0) + 1\n"
            "    if KZBOTH == 3 then\n"
            "      KZOUT = 'coop: both players dead, mission failed'\n"
            // as the level library's EndIfPlayerOrBuddyDied does: lost status + gIsFinished (read by the level's IsFinished)
            "      call(function() gGamePlayInfo:SetGameStatus(STATUS_LOST) gIsFinished = TRUE end, {}, 'x', function(e) KZOUT = 'coop: lost ERROR ' .. tostring(e) end)\n"
            "    end\n"
            "  else\n"
            "    KZBOTH = 0\n"
            "    if d0 then\n"
            "      KZDEAD0 = (KZDEAD0 or 0) + 1\n"
            "      if KZDEAD0 == 1 then KZOUT = 'p1: dead' end\n"
            "      if KZDEAD0 >= " + std::to_string(respawnSec) + " then\n"
            "        KZDEAD0 = 0\n"
            // two steps: the C++ side first keeps the dead actor alive (kzCoopFrame), then runs the respawn
            "        KZOUT = 'p1: respawn now'\n"
            "      end\n"
            "    else\n"
            "      KZDEAD0 = 0\n"
            "    end\n"
            "  end\n"
            "  local a = a1\n"
            "  if not (d0 and d1) and (a == nil or not a:IsAlive()) then\n"
            "    if (KZDEAD or 0) == 0 then KZOUT = 'p2: dead' end\n"
            "    KZDEAD = (KZDEAD or 0) + 1\n"
            "    if KZDEAD >= " + std::to_string(respawnSec) + " and not gGamePlayInfo:IsRespawning(1) then\n"
            "      KZDEAD = 0\n"
            // Not gGamePlayInfo:Respawn: it runs the slot's creation function in the GAME MODE script state and stores
            // what it returns as the slot's actor (FUN_001b96c8, slot +0xD4); in the campaign that call fails and the
            // slot gets a stale pointer (hang on the next method call). AddPlayer again in the level state, as when
            // player 2 joined, replaces the slot's actor (verified: GetPlayerActor(1) = the new one, alive).
            "      KZP2 = KZADDP2(KZSPAWN or 'Spawn_Player', 1)\n"
            "      KZOUT = 'p2: respawned'\n"
            "    end\n"
            "  else\n"
            "    KZDEAD = 0\n"
            "  end\n"
            "end\n"
            "end, {}, 'x', function(e) KZOUT = 'watchdog ERROR ' .. tostring(e) end)\n");
    // The other player left (point 1): its character dies on this PC and is removed two seconds later (Remove right away
    // crashed the host; Die first is how the game's own squad code does it, KillSquad/RemoveSquad). The globals that
    // pointed at it (PLAYER on the joiner, where the host's player 1 leaves) move to the player who stays.
    // KZ_COOP_LEAVE_TEST=<frame> simulates player <1 - local> leaving at that frame (local latch tests).
    static const uint32_t leaveTest = std::getenv("KZ_COOP_LEAVE_TEST") ? static_cast<uint32_t>(std::atoi(std::getenv("KZ_COOP_LEAVE_TEST"))) : 0;
    static uint32_t removeAt = 0;
    static bool hadP2 = false; // player 2 was added (a session that ends at frame 1 never had one)
    if (frame == 30 && kzNetHasPlayer2() && !noAuto)
        hadP2 = true;
    const bool leaveNow = (kzNetPeerLeft() && hadP2 && frame > 30 && !left && removeAt == 0) || (leaveTest && frame == leaveTest);
    if (leaveNow)
    {
        left = true;
        static const int forced = std::getenv("KZ_COOP_VIEW") ? std::atoi(std::getenv("KZ_COOP_VIEW")) : -1;
        const int me = forced >= 0 ? forced : kzNetLocalPlayer();
        const std::string gone = std::to_string(1 - me), stay = std::to_string(me);
        g_luaQueue.push_back(
            "KZOUT = 'leave: nothing to remove'\n"
            "if KZFIX and gGamePlayInfo:GetNrOfPlayers() >= 2 then\n"
            "  local gone = gGamePlayInfo:GetPlayerActor(" + gone + ")\n"
            "  local stay = gGamePlayInfo:GetPlayerActor(" + stay + ")\n"
            "  if gone and stay then\n"
            "    KZFIX(gone, stay)\n"
            "    KZP1 = stay\n"
            "    KZGONE = gone\n"
            "    call(function() KZGONE:Die() end, {}, 'x', function(e) KZOUT = 'leave: Die ERROR ' .. tostring(e) end)\n"
            "    if KZOUT == 'leave: nothing to remove' then KZOUT = 'leave: player " + gone + " removed' end\n"
            // point 5: on-screen message in the game's own HUD notification style (shown on the remaining player)
            "    KZSTAY = stay\n" // Lua 4: an anonymous function cannot see the outer locals
            "    call(function() gGameInterface:AddNotification(KZSTAY, '" + std::string(me == 0 ? "JUGADOR 2 DESCONECTADO" : "ANFITRION DESCONECTADO") + "', 5, 0, 0) end, {}, 'x', function(e) end)\n"
            "  end\n"
            "end\n");
        removeAt = frame + 60;
    }
    if (removeAt && frame >= removeAt)
    {
        removeAt = 0;
        g_luaQueue.push_back("KZOUT = nil\nif KZGONE then\n"
                             "  KZOUT = 'leave: body removed'\n"
                             "  call(function() KZGONE:Remove() end, {}, 'x', function(e) KZOUT = 'leave: Remove ERROR ' .. tostring(e) end)\n"
                             "  KZGONE = nil\n"
                             "end\n");
    }
    if (g_luaQueue.empty())
        return false;
    const std::string code = g_luaQueue.front();
    g_luaQueue.pop_front();
    return startLua(rdram, ctx, rt, code);
}

uint32_t kzCoopPlayer2() { return 0; }

namespace
{
    // KZ_ARGLOG=<hex addr>: logs every call of that guest function with a0..a3, t0..t3 (EABI argument registers) and the
    // text they point to when it looks like a string (RE aid).
    PS2Runtime::RecompiledFunction g_argOrig = nullptr;
    uint32_t g_argAddr = 0;
    void argLogWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        std::string line = "[kz] arglog " + std::to_string(g_argAddr);
        char buf[96];
        const int regs[8] = {4, 5, 6, 7, 8, 9, 10, 11};
        for (int r : regs)
        {
            const uint32_t v = reg(ctx, r);
            std::snprintf(buf, sizeof(buf), " r%d=%x", r, v);
            line += buf;
            if (v >= 0x100000u && v < 0x02000000u)
            {
                const std::string s = cstr(rdram, v, 48);
                bool text = s.size() >= 2;
                for (char c : s)
                    text = text && c >= 32 && c < 127;
                if (text)
                    line += "\"" + s + "\"";
            }
        }
        std::cout << line << " ra=" << std::hex << reg(ctx, 31) << std::dec << std::endl;
        g_argOrig(rdram, ctx, rt);
        std::cout << "[kz] arglog ret v0=" << std::hex << reg(ctx, 2) << std::dec << std::endl;
    }
}

void kzCoopArgLogInstall(PS2Runtime &runtime)
{
    if (const char *e = std::getenv("KZ_ARGLOG"))
    {
        g_argAddr = static_cast<uint32_t>(std::strtoul(e, nullptr, 16));
        g_argOrig = runtime.lookupFunction(g_argAddr);
        if (!g_argOrig || !runtime.replaceFunction(g_argAddr, &argLogWrap))
            std::cerr << "[kz] arglog hook failed" << std::endl;
    }
}

// ---- Per-PC view (point 3). The main frame function (ES FUN_001c03c8) renders every player slot that has a camera
// (+0xE4) and a render camera (+0xE8): world (FUN_001c1be0), then that slot's HUD in its viewport (+0xF0 x, +0xF4 y,
// +0xF8 right, +0xFC bottom; with two players the game sets top/bottom halves). The loop starts after FUN_001c1b58 and
// ends at FUN_001c0b50 (both called once per frame from it). Around the loop every PC hides the other player's slot
// (camera words zeroed) and gives its own slot the full screen; everything is put back right after, so the simulation
// never sees the change. KZ_COOP_VIEW=0|1 forces which player this PC shows (tests); default kzNetLocalPlayer().
namespace
{
    struct ViewAddrs
    {
        uint32_t preLoop = 0, postLoop = 0;
    };
    ViewAddrs viewAddrs()
    {
        ViewAddrs v;
        if (kzIsEs())
        {
            v.preLoop = 0x001C1B58u;
            v.postLoop = 0x001C0B50u;
        }
        return v;
    }
    PS2Runtime::RecompiledFunction g_origPreLoop = nullptr, g_origPostLoop = nullptr;
    struct HiddenSlot
    {
        bool active = false;
        uint32_t other = 0, cam = 0, rcam = 0;
        uint32_t local = 0, vp[4] = {};
    } g_hidden;
    int g_forceView = -1;
    constexpr uint32_t kRngAddrEs = 0x0055B300u; // global LCG state (kz_lockstep rngAddr)
    uint32_t g_rngSaved = 0;
    bool g_rngSavedValid = false;

    void restoreSlots(uint8_t *rdram);
    // View mode (KZ_COOP_VIEWMODE): "mute" (default) keeps the guest's rendering exactly the original's on both PCs
    // (same simulation by construction) and drops, on the host GS only, the GIF packets of the draws this PC must not
    // show: the host drops player 2's slot (world + HUD), the joiner player 1's HUD (player 2's world is drawn last and
    // covers player 1's anyway). Only valid when each slot's packets are submitted while the guest renders it.
    // "swap" reorders the slots instead (guest-visible: changes the simulation slightly, kept for experiments).
    int g_viewMode = 0; // 0 natural (default: the slot created last, player 2, is shown on both PCs), 1 swap (KZ_COOP_VIEWMODE=swap)
    bool g_loopActive = false;
    uint32_t g_loopRcam[2] = {};
    int g_probeLines = 0;
    bool g_probe = false;
    PS2Runtime::RecompiledFunction g_origWorld = nullptr;
    // KZ_COOP_LISTLOG=N: logs the display list write pointer (scratchpad 0x3D3C) around the frame stages, N lines.
    int g_listLog = 0;
    // Host view: the slot loop appends each slot's part of the display list in turn (the list write pointer, scratchpad
    // 0x3D3C, moves between the world calls, not inside them). The tags written from player 2's world call to the end
    // of the loop are player 2's view; on the PC that shows player 1 the runtime drops their GS output when the list is
    // sent (ps2_memory.cpp g_ps2GsMuteList). The guest builds and sends the full list, so the simulation is untouched.
    int g_muteView = -1;        // slot whose part of the list this PC drops (-1 none): 1 on the host, 0 on the joiner
    uint32_t g_slotPtr[2] = {}; // list pointer when each slot's world call starts
    uint32_t g_muteSlot = 0;
    uint32_t listPtr(PS2Runtime *rt)
    {
        uint32_t p;
        std::memcpy(&p, rt->memory().getScratchpad() + 0x3D3C, 4);
        return p & 0x01FFFFFFu;
    }
    void listLog(PS2Runtime *rt, const char *what, int idx = -1)
    {
        if (g_listLog <= 0)
            return;
        --g_listLog;
        uint32_t p;
        std::memcpy(&p, rt->memory().getScratchpad() + 0x3D3C, 4);
        std::fprintf(stderr, "[kz] listlog %s %d ptr %08x\n", what, idx, p);
    }
    void worldWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        listLog(rt, "world-in", g_loopActive ? (getRegU32(ctx, 7) == g_loopRcam[0] ? 0 : 1) : -1);
        if (g_muteView >= 0 && g_loopActive)
            for (int i = 0; i < 2; ++i)
                if (getRegU32(ctx, 7) == g_loopRcam[i] && !g_slotPtr[i])
                    g_slotPtr[i] = listPtr(rt) & ~15u;
        const uint32_t rcam = getRegU32(ctx, 7);
        const int idx = !g_loopActive ? -1 : rcam == g_loopRcam[0] ? 0 : rcam == g_loopRcam[1] ? 1 : -1;
        const uint32_t before = g_ps2GifSubmits.load();
        // probe: words (RAM globals 0x500000..0x600000, scratchpad) pointing into the display list buffers that change
        // while one slot renders: the list's write pointer.
        static std::vector<uint32_t> snapG, snapS;
        const bool scan = g_probe && g_probeLines >= 13 && g_probeLines < 40 && g_loopActive;
        auto inList = [](uint32_t v) { v &= 0x0FFFFFFFu; return (v >= 0x5DB700u && v < 0x69B800u + 0xC0000u); };
        uint8_t *spr = rt->memory().getScratchpad();
        if (scan)
        {
            snapG.assign(reinterpret_cast<uint32_t *>(rdram + 0x500000), reinterpret_cast<uint32_t *>(rdram + 0x600000));
            snapS.assign(reinterpret_cast<uint32_t *>(spr), reinterpret_cast<uint32_t *>(spr + 0x4000));
        }
        // KZ_COOP_SKIPWORLD=1 (experiment, perf): the slot this PC does not show is not rendered at all (its world call
        // is skipped on this PC only). Valid only if the world render never feeds the simulation (checked by comparing
        // the state of a PC showing player 1 with one showing player 2).
        static const bool skipWorld = std::getenv("KZ_COOP_SKIPWORLD") && *std::getenv("KZ_COOP_SKIPWORLD") == '1';
        if (skipWorld && idx >= 0 && idx == g_muteView)
        {
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
        g_origWorld(rdram, ctx, rt);
        listLog(rt, "world-out", idx);
        if (scan && ctx->pc == getRegU32(ctx, 31))
        {
            const uint32_t *g = reinterpret_cast<const uint32_t *>(rdram + 0x500000);
            const uint32_t *sp = reinterpret_cast<const uint32_t *>(spr);
            for (size_t i = 0; i < snapG.size(); ++i)
                if (g[i] != snapG[i] && inList(g[i]) && inList(snapG[i]))
                    std::fprintf(stderr, "[kz] gsprobe slot %d ram %06zx %08x -> %08x\n", idx, 0x500000 + i * 4, snapG[i], g[i]);
            for (size_t i = 0; i < snapS.size(); ++i)
                if (sp[i] != snapS[i] && inList(sp[i]) && inList(snapS[i]))
                    std::fprintf(stderr, "[kz] gsprobe slot %d spr %04zx %08x -> %08x\n", idx, i * 4, snapS[i], sp[i]);
        }
        if (g_probe && g_probeLines < 60 && g_loopActive)
        {
            ++g_probeLines;
            std::cerr << "[kz] gsprobe world slot " << idx << " packets " << (g_ps2GifSubmits.load() - before) << std::endl;
        }
    }
    void swapSlots(uint8_t *rdram, uint32_t a, uint32_t b)
    {
        uint8_t tmp[0x1E0];
        std::memcpy(tmp, rdram + (a & kRamMask), sizeof(tmp));
        std::memcpy(rdram + (a & kRamMask), rdram + (b & kRamMask), sizeof(tmp));
        std::memcpy(rdram + (b & kRamMask), tmp, sizeof(tmp));
    }
    // The split-screen layout halves each player camera's field of view (camera +0x70, 56.25 -> 28.125) and doubles its
    // aspect ratio (+0x74, 16:9 -> 32:9) for a half-height view; the game writes them again every frame during the
    // simulation. Every view is full screen here, so right before rendering they get the single-player values back.
    // Same on every PC.
    void fullScreenCameras(uint8_t *rdram)
    {
        for (int i = 0; i < 2; ++i)
        {
            const uint32_t s = kzCoopSlot(rdram, i);
            if (!s || !kzCoopSlot(rdram, 1))
                return;
            const uint32_t cam = rd32(rdram, s + 0xE4);
            if (cam < 0x100000u || cam >= 0x02000000u)
                continue;
            float fov, asp;
            std::memcpy(&fov, rdram + cam + 0x70, 4);
            std::memcpy(&asp, rdram + cam + 0x74, 4);
            if (asp > 2.5f) // still the split-screen value
            {
                fov *= 2.0f;
                asp *= 0.5f;
                std::memcpy(rdram + cam + 0x70, &fov, 4);
                std::memcpy(rdram + cam + 0x74, &asp, 4);
            }
        }
    }
    void preLoopWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        restoreSlots(rdram); // the previous frame skipped the loop (and its end)
        static const bool keepSplitCam = std::getenv("KZ_COOP_SPLITCAM") != nullptr;
        if (!keepSplitCam)
            fullScreenCameras(rdram);
        if (g_viewMode == 0)
        {
            const uint32_t s0 = kzCoopSlot(rdram, 0), s1 = kzCoopSlot(rdram, 1);
            g_loopActive = s0 && s1;
            g_loopRcam[0] = s0 ? rd32(rdram, s0 + 0xE8) : 0;
            g_loopRcam[1] = s1 ? rd32(rdram, s1 + 0xE8) : 0;
            g_ps2GsMute.store(false);
            // Host view: the display list holds one segment per slot (slot 0 first) and the last one covers the screen.
            // On the PC that shows player 1, the runtime drops the GS output of the second segment (ps2_memory.cpp,
            // g_ps2GsMuteList, set in postLoopWrap): the guest still builds and sends everything (simulation untouched).
            const int me = g_forceView >= 0 ? g_forceView : kzNetLocalPlayer();
            // The joiner drops player 1's part too: player 2's view is drawn after it over the whole screen.
            g_muteView = !g_loopActive ? -1 : me == 0 ? 1 : 0;
            g_slotPtr[0] = g_slotPtr[1] = 0;
            static const bool viewLog = std::getenv("KZ_COOP_VIEWLOG") != nullptr;
            static uint32_t viewLogN = 0;
            if (viewLog && g_loopActive && (++viewLogN % 1u) == 0u)
                std::cerr << "[kz] coop view: me " << me << ", view starts in last list " << g_ps2ViewStarts.load() << std::endl;
            if (g_probe && g_probeLines < 60 && g_loopActive)
            {
                ++g_probeLines;
                std::cerr << "[kz] gsprobe loop start, packets so far " << g_ps2GifSubmits.load() << std::endl;
                if (g_probeLines == 13)
                    g_ps2KickProbe.store(80); // the DMA starts of the next frames (ps2_memory.cpp)
            }
            listLog(rt, "pre-in");
            g_origPreLoop(rdram, ctx, rt);
            listLog(rt, "pre-out");
            return;
        }
        // What gets drawn differs per PC (each shows its own player), and drawing code steps the global random generator
        // (particles, inline copies of the LCG): its state is saved here and put back after the loop, so rendering never
        // changes the simulation's random sequence. Only with two players (single player keeps the original behaviour).
        if (kzCoopSlot(rdram, 1))
        {
            g_rngSaved = rd32(rdram, kRngAddrEs);
            g_rngSavedValid = kzIsEs();
        }
        const int me = g_forceView >= 0 ? g_forceView : kzNetLocalPlayer();
        const uint32_t s0 = kzCoopSlot(rdram, 0), s1 = kzCoopSlot(rdram, 1);
        g_hidden.active = false;
        // Both slots are still rendered every frame on every PC, as the original does with two players (rendering has
        // side effects the simulation sees: first-person resources, animation detail...). Only the order changes: the
        // slot drawn last fills the screen, so the host swaps the two slots for the loop and its own view comes last.
        if (s0 && s1 && rd32(rdram, s1 + 0xE4) && rd32(rdram, s1 + 0xE8) && rd32(rdram, s0 + 0xE4) && me == 0)
        {
            g_hidden.active = true;
            g_hidden.other = s1;
            g_hidden.local = s0;
            swapSlots(rdram, s0, s1);
        }
        g_origPreLoop(rdram, ctx, rt);
    }
    void restoreSlots(uint8_t *rdram)
    {
        if (g_rngSavedValid)
        {
            wr32(rdram, kRngAddrEs, g_rngSaved);
            g_rngSavedValid = false;
        }
        if (!g_hidden.active)
            return;
        g_hidden.active = false;
        swapSlots(rdram, g_hidden.local, g_hidden.other);
        // The loop leaves the last rendered camera as the "current camera" ([[game]+0x88]+0x34, FUN_003ca190) and the
        // simulation reads it next frame: put back the one the original order leaves (the last slot's).
        const uint32_t g = rd32(rdram, kzAddrs().gameObjectPtr);
        if (g >= 0x100000u && g < 0x02000000u)
            if (const uint32_t cm = rd32(rdram, g + 0x88); cm >= 0x100000u && cm < 0x02000000u)
                wr32(rdram, cm + 0x34, rd32(rdram, g_hidden.other + 0xE4));
    }
    void postLoopWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (g_probe && g_probeLines < 60 && g_loopActive)
        {
            ++g_probeLines;
            std::cerr << "[kz] gsprobe loop end, packets so far " << g_ps2GifSubmits.load() << std::endl;
        }
        g_loopActive = false;
        g_ps2GsMute.store(false);
        restoreSlots(rdram);
        listLog(rt, "post-in");
        {
            // this frame's list goes to the next free entry (two lists in flight at most)
            uint32_t lo = 0, hi = 0;
            if (g_muteView == 1 && g_slotPtr[1])
            {
                lo = g_slotPtr[1];
                hi = listPtr(rt);
            }
            else if (g_muteView == 0 && g_slotPtr[0] && g_slotPtr[1])
            {
                lo = g_slotPtr[0];
                hi = g_slotPtr[1];
            }
            g_muteSlot ^= 1u;
            g_ps2GsMuteList[g_muteSlot][1].store(0);
            g_ps2GsMuteList[g_muteSlot][0].store(lo);
            g_ps2GsMuteList[g_muteSlot][1].store(hi > lo ? hi : 0u);
            g_slotPtr[0] = g_slotPtr[1] = 0;
        }
        g_origPostLoop(rdram, ctx, rt);
        listLog(rt, "post-out");
    }
}

void kzCoopViewInstall(PS2Runtime &runtime)
{
    if (const char *e = std::getenv("KZ_COOP_VIEWMODE"); e && _stricmp(e, "swap") == 0)
        g_viewMode = 1;
    g_probe = std::getenv("KZ_COOP_GSPROBE") != nullptr;
    // KZ_COOP_MUTE: default: the hidden view's part of the list still runs through VIF/VU1 and its GS packets keep their
    // texture uploads and register writes, only the primitives are not drawn (ps2_memory.cpp). "skip": the host does not
    // process those bytes at all (a few fps faster, but texture uploads in the hidden part are lost: HUD glitches).
    const char *muteMode = std::getenv("KZ_COOP_MUTE");
    g_ps2MuteSkip.store(muteMode && _stricmp(muteMode, "skip") == 0);
    if (const char *e = std::getenv("KZ_COOP_LISTLOG"))
        g_listLog = std::atoi(e);
    if (kzIsEs())
    {
        g_origWorld = runtime.lookupFunction(kzAddrs().world);
        if (!g_origWorld || !runtime.replaceFunction(kzAddrs().world, &worldWrap))
            std::cerr << "[kz] coop: world hook failed" << std::endl;
    }
    if (const char *e = std::getenv("KZ_COOP_VIEW"))
        g_forceView = std::atoi(e);
    const ViewAddrs v = viewAddrs();
    if (!v.preLoop)
        return;
    g_origPreLoop = runtime.lookupFunction(v.preLoop);
    g_origPostLoop = runtime.lookupFunction(v.postLoop);
    if (!g_origPreLoop || !g_origPostLoop || !runtime.replaceFunction(v.preLoop, &preLoopWrap) ||
        !runtime.replaceFunction(v.postLoop, &postLoopWrap))
        std::cerr << "[kz] coop: view hooks failed" << std::endl;
}

// ---- Player 2's character (point 3). The engine loads the model of every configured player when a level loads (verified:
// with gGamePlayInfo set to 2 players and player 2 = Luger or ISAonline before the load, AddPlayer works in level 1;
// configured after the load, CreatePlayer hangs). So player 2 is configured at the moments the front end configures
// player 1, before the load: gGamePlayInfo::SetLevelFile (level chosen, next level) and SetPlayerCharacter(0, ...)
// (character select). Same on both PCs (the menus run on the host's input stream, the choice travels in HELLO).
// gGamePlayInfo (ES): SetNrOfPlayers FUN_001b92f0(this, n), SetPlayerCharacter FUN_001b9aa8(this, i, str),
// SetLevelFile FUN_001b8ad8(this, str) -> level file at this+0xC. Slots this+0x220 (0x1E0 each, count +0x218):
// +0x40 character, +0x60 faction, +0x84 joystick, +0x89 creation function (FUN_001b8520 clears them).
namespace
{
    PS2Runtime::RecompiledFunction g_origSetNr = nullptr, g_origSetChar = nullptr, g_origSetLevel = nullptr;
    bool g_inP2Config = false;

    // Story rule (Santi): chapters where only Templar is playable get a generic ISA soldier; later chapters the
    // joiner's choice once that protagonist has joined (Luger end of ch. 2, Rico ch. 3, Hakha end of ch. 4).
    std::string p2Character(const std::string &levelFile)
    {
        static const char *names[4] = {"Templar", "Luger", "Choyan", "Hakha"};
        if (const char *e = std::getenv("KZ_COOP_P2MODEL"); e && *e) // tests: force a model
            return e;
        const int chapter = std::atoi(levelFile.c_str());
        const uint32_t choice = kzNetPlayer2Char() & 3u;
        const int joinsAt[4] = {3, 3, 4, 5}; // first chapter where the joiner may play that protagonist
        if (chapter >= joinsAt[choice])
            return names[choice];
        return "ISAonline";
    }

    void writeStr(uint8_t *rdram, uint32_t a, const std::string &s, size_t max)
    {
        const size_t n = std::min(s.size(), max - 1);
        std::memcpy(rdram + (a & kRamMask), s.data(), n);
        rdram[(a + n) & kRamMask] = 0;
    }

    bool g_afterLevelChange = false;
    void configureP2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t gpi)
    {
        if (!kzNetSessionMode() || g_inP2Config || !g_origSetNr || g_levelChangePending)
            return;
        if (static_cast<int>(rd32(rdram, gpi + 0x218)) < 2)
        {
            // call the game's SetNrOfPlayers(this, 2): it allocates and initialises the slot
            g_inP2Config = true;
            const uint32_t a0 = reg(ctx, 4), a1 = reg(ctx, 5), ra = reg(ctx, 31), pc = ctx->pc;
            setReg(ctx, 4, gpi);
            setReg(ctx, 5, 2);
            setReg(ctx, 31, pc);
            ctx->pc = 0x001B92F0u;
            g_origSetNr(rdram, ctx, rt);
            setReg(ctx, 4, a0);
            setReg(ctx, 5, a1);
            setReg(ctx, 31, ra);
            ctx->pc = pc;
            g_inP2Config = false;
        }
        const uint32_t slots = rd32(rdram, gpi + 0x220);
        if (static_cast<int>(rd32(rdram, gpi + 0x218)) < 2 || slots < 0x100000u || slots >= 0x02000000u)
            return;
        const uint32_t s0 = slots, s1 = slots + 0x1E0u;
        const std::string level = cstr(rdram, gpi + 0xC, 0x60);
        // After a level change in this session only player 1's character is loaded by the new level (see setLevelWrap):
        // player 2 takes it (two Templars instead of a crash). A new game from the menu chooses again.
        static const bool sameCharAfterChange = std::getenv("KZ_COOP_P2CHAR_AFTER_LEVEL") == nullptr; // the chosen model is not loaded by a level entered from the previous one
        std::string ch = g_afterLevelChange && sameCharAfterChange && !kzCoopNative() ? cstr(rdram, s0 + 0x40, 0x20) : p2Character(level);
        // 11-05: the generic online soldier is an extra model the bigger levels cannot hold (1.LightIndustry with player 2
        // as ISAonline crashed both copies while loading, in the effect renderer FUN_0031aa68 / malloc; as player 1's
        // character it loads and plays). Player 2 takes player 1's character (already loaded, no extra memory) unless
        // KZ_COOP_P2_ISAONLINE=1.
        static const bool allowIsaOnline = std::getenv("KZ_COOP_P2_ISAONLINE") != nullptr;
        if (kzCoopNative() && ch == "ISAonline" && !allowIsaOnline && !cstr(rdram, s0 + 0x40, 0x20).empty())
            ch = cstr(rdram, s0 + 0x40, 0x20);
        writeStr(rdram, s1 + 0x40, ch, 0x20);
        writeStr(rdram, s1 + 0x60, cstr(rdram, s0 + 0x60, 16).empty() ? std::string("ISA") : cstr(rdram, s0 + 0x60, 16), 16);
        wr32(rdram, s1 + 0x84, 1);
        // SetPlayerControlType(1, 'Human') (FUN_004368a8: char[16] at slot +0x70), as the Battlefields menu does for every
        // human player: the level load prepares 'Human' slots as local players (FUN_001becc0 -> FUN_001b9928)
        if (kzCoopNative())
            writeStr(rdram, s1 + 0x70, "Human", 16);
        writeStr(rdram, s1 + 0x89, cstr(rdram, s0 + 0x89, 0x40).empty() ? std::string("AddPlayer") : cstr(rdram, s0 + 0x89, 0x40), 0x40);
        static std::string lastLog;
        const std::string log = level + " -> " + ch;
        if (log != lastLog)
        {
            lastLog = log;
            std::cout << "[kz] coop: player 2 configured for level " << level << ": " << ch << std::endl;
        }
    }

    void setNrWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        // a 1-player configuration made by the front end while a session runs stays at 2 (0 = the game shutting down)
        if (kzNetSessionMode() && !g_inP2Config && !g_levelChangePending && static_cast<int>(reg(ctx, 5)) == 1)
            setReg(ctx, 5, 2);
        const uint32_t gpi = reg(ctx, 4);
        g_origSetNr(rdram, ctx, rt);
        if (!g_inP2Config)
            configureP2(rdram, ctx, rt, gpi);
    }
    void setCharWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t gpi = reg(ctx, 4), idx = reg(ctx, 5);
        g_origSetChar(rdram, ctx, rt);
        if (idx == 0)
            configureP2(rdram, ctx, rt, gpi);
    }
    PS2Runtime::RecompiledFunction g_setSlotPlayer = nullptr;
    void callSetNr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t gpi, uint32_t n);
    void setLevelWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t gpi = reg(ctx, 4);
        // Next level (Santi + friend 11-04: level 1 -> 2 crashed both PCs while loading): player 2's slot still held
        // its level-1 actor/controller, and the game's player creation at the new level used them (CreatePlayer ES
        // FUN_001cc010 got a null actor). Empty the slot the game's own way first, as before a checkpoint restore;
        // the watchdog adds player 2 again in the new level.
        if (!kzCoopNative() && kzNetSessionMode() && g_setSlotPlayer && gpi >= 0x100000u && gpi < 0x02000000u &&
            static_cast<int>(rd32(rdram, gpi + 0x218)) >= 2)
            if (const uint32_t slots = rd32(rdram, gpi + 0x220); slots >= 0x100000u && slots < 0x02000000u &&
                                                                  (rd32(rdram, slots + 0x1E0 + 0xD4) || rd32(rdram, slots + 0x1E0 + 0xD8)))
            {
                const uint32_t a0 = reg(ctx, 4), a1 = reg(ctx, 5), a2 = reg(ctx, 6), a3 = reg(ctx, 7), ra = reg(ctx, 31), pc = ctx->pc;
                setReg(ctx, 4, gpi);
                setReg(ctx, 5, 1);
                setReg(ctx, 6, 0);
                setReg(ctx, 7, 0);
                setReg(ctx, 31, pc);
                ctx->pc = 0x001B9E00u;
                g_setSlotPlayer(rdram, ctx, rt);
                setReg(ctx, 4, a0);
                setReg(ctx, 5, a1);
                setReg(ctx, 6, a2);
                setReg(ctx, 7, a3);
                setReg(ctx, 31, ra);
                ctx->pc = pc;
                g_hiddenCtlSlot = g_hiddenCtl = 0;
                // The new level's own AddPlayers would create player 2 during the load, but a character other than
                // player 1's is not loaded there (Player.3rd.ISAonline -> null actor -> crash): one player for the
                // load; at game frame 30 the coop add brings player 2 back with player 1's character (always loaded).
                // The new level's own AddPlayers creates player 2 during the load at the level's player spawn, but only
                // player 1's character is loaded there: player 2 takes it (configureP2, g_afterLevelChange). Removing
                // the slot instead (SetNrOfPlayers(1), re-added after the load) lost its +0x100 state object (crash in
                // FUN_001b9d18 at the next checkpoint save).
                g_afterLevelChange = true;
                std::cout << "[kz] coop: next level, player 2 detached from its slot first" << std::endl;
            }
        g_origSetLevel(rdram, ctx, rt);
        configureP2(rdram, ctx, rt, gpi);
    }

    // Checkpoint restore (ES FUN_001c5f08, also the "Retry" of a failed mission): rebuilds the world from the checkpoint
    // stream and then restores the saved players' slot data (count from the stream). The level-start checkpoint is saved
    // before player 2 is added, so player 2's slot kept pointers to its old actor/controller/camera, which the world
    // rebuild destroys: later releases and uses broke the heap (both games hung in malloc, black screen). Before the
    // restore the slot is destroyed the game's own way (SetNrOfPlayers(1)); after it, 2 players again (configureP2) and
    // the watchdog brings player 2 back (the level is not reloaded: its model stays cached).
    PS2Runtime::RecompiledFunction g_origCpRestore = nullptr;
    void callSetNr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t gpi, uint32_t n)
    {
        g_inP2Config = true;
        const uint32_t a0 = reg(ctx, 4), a1 = reg(ctx, 5), ra = reg(ctx, 31), pc = ctx->pc;
        setReg(ctx, 4, gpi);
        setReg(ctx, 5, n);
        setReg(ctx, 31, pc);
        ctx->pc = 0x001B92F0u;
        g_origSetNr(rdram, ctx, rt);
        setReg(ctx, 4, a0);
        setReg(ctx, 5, a1);
        setReg(ctx, 31, ra);
        ctx->pc = pc;
        g_inP2Config = false;
    }
    // (Verified with Ghidra) the checkpoint save FUN_001c5798 writes each slot's controller (+0xD8) and +0x100 state only
    // when the slot has a controller, and the restore FUN_001c5f08 reads them when the slot has one AT RESTORE TIME. The
    // level-start checkpoint is saved before player 2 exists (no controller), the restore runs with player 2 present: it
    // read data that was never written, the stream went out of step and the rest of the restore broke the heap (both
    // games hung in malloc, black screen after Retry / Restart checkpoint). Player 2's controller is hidden (+0xD8 = 0)
    // around both, so its state is never in the stream: the restore keeps player 2 as it is.
    PS2Runtime::RecompiledFunction g_origCpSave = nullptr;
    template <PS2Runtime::RecompiledFunction *Orig>
    void hideP2Controller(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, const char *what)
    {
        const uint32_t game = reg(ctx, 4);
        const uint32_t gpi = game >= 0x100000u && game < 0x02000000u ? rd32(rdram, game + 0x74) : 0;
        uint32_t s1 = 0, ctl = 0;
        if (gpi >= 0x100000u && gpi < 0x02000000u && static_cast<int>(rd32(rdram, gpi + 0x218)) >= 2)
            if (const uint32_t slots = rd32(rdram, gpi + 0x220); slots >= 0x100000u && slots < 0x02000000u)
            {
                s1 = slots + 0x1E0u;
                ctl = rd32(rdram, s1 + 0xD8);
                wr32(rdram, s1 + 0xD8, 0);
            }
        (*Orig)(rdram, ctx, rt);
        if (s1)
        {
            // Both functions yield to other guest threads while they stream: the rest of the save/restore runs after
            // this wrapper returned. The controller comes back at the next game frame (kzCoopFrame), when it is over.
            if (ctl && !g_hiddenCtlSlot)
            {
                g_hiddenCtlSlot = s1;
                g_hiddenCtl = ctl;
            }
            std::cout << "[kz] coop: checkpoint " << what << " without player 2's slot data" << std::endl;
        }
    }
    // Restore: the world rebuild destroys player 2's actor and controller, which its slot still referenced (AddPlayer
    // later released the destroyed actor: jump through a freed vtable in FUN_00244088). So before the restore the
    // slot is emptied the game's own way: FUN_001b9e00(gamePlayInfo, slot, actor = 0, controller = 0) releases both
    // references and removes the slot's camera. The watchdog then brings player 2 back next to player 1.

    // On-screen messages (objective added/completed, AddNotification...) go to one player's queue: FUN_001bb668(f12..f14,
    // gamePlayInfo, player index, text) appends to slot +0x1D0/+0x1D8. Objective messages always pass player 0
    // (FUN_001cd270), so player 2 never saw them: every message for player 0 also goes to player 1 (both PCs alike).
    PS2Runtime::RecompiledFunction g_origHudMsg = nullptr;
    void hudMsgWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t gpi = reg(ctx, 4), idx = reg(ctx, 5), text = reg(ctx, 6), ra = reg(ctx, 31);
        const float f12 = ctx->f[12], f13 = ctx->f[13], f14 = ctx->f[14];
        g_origHudMsg(rdram, ctx, rt);
        if (idx == 0 && kzNetSessionMode() && ctx->pc == ra && gpi >= 0x100000u && gpi < 0x02000000u &&
            static_cast<int>(rd32(rdram, gpi + 0x218)) >= 2)
        {
            const uint32_t v0 = reg(ctx, 2);
            setReg(ctx, 4, gpi);
            setReg(ctx, 5, 1);
            setReg(ctx, 6, text);
            setReg(ctx, 31, ra);
            ctx->f[12] = f12;
            ctx->f[13] = f13;
            ctx->f[14] = f14;
            ctx->pc = 0x001BB668u;
            g_origHudMsg(rdram, ctx, rt);
            setReg(ctx, 2, v0);
        }
    }
    void cpRestoreWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t game = reg(ctx, 4);
        const uint32_t gpi = game >= 0x100000u && game < 0x02000000u ? rd32(rdram, game + 0x74) : 0;
        if (!kzCoopNative() && g_setSlotPlayer && gpi >= 0x100000u && gpi < 0x02000000u && static_cast<int>(rd32(rdram, gpi + 0x218)) >= 2)
        {
            const uint32_t a0 = reg(ctx, 4), a1 = reg(ctx, 5), a2 = reg(ctx, 6), a3 = reg(ctx, 7), ra = reg(ctx, 31), pc = ctx->pc;
            setReg(ctx, 4, gpi);
            setReg(ctx, 5, 1);
            setReg(ctx, 6, 0);
            setReg(ctx, 7, 0);
            setReg(ctx, 31, pc);
            ctx->pc = 0x001B9E00u;
            g_setSlotPlayer(rdram, ctx, rt);
            setReg(ctx, 4, a0);
            setReg(ctx, 5, a1);
            setReg(ctx, 6, a2);
            setReg(ctx, 7, a3);
            setReg(ctx, 31, ra);
            ctx->pc = pc;
            g_hiddenCtlSlot = g_hiddenCtl = 0; // nothing to put back
            std::cout << "[kz] coop: checkpoint restore, player 2 detached from its slot first" << std::endl;
        }
        g_origCpRestore(rdram, ctx, rt);
    }
    void cpSaveWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (g_levelChangePending)
        {
            g_levelChangePending = false;
            g_addP2At = g_frameNow + 30;
        }
        if (kzCoopNative())
            g_origCpSave(rdram, ctx, rt); // player 2 is a normal player: saved with its slot data like player 1
        else
            hideP2Controller<&g_origCpSave>(rdram, ctx, rt, "save");
    }
    // Test (KZ_COOP_CPSAVE_TEST=<frame>): from that game frame the per-frame checkpoint check (FUN_001c55e8) sees the
    // player inside the first active checkpoint area once (FUN_00272160 answers 1): the game saves its own way, as a
    // checkpoint reached mid-level with player 2 present.
    PS2Runtime::RecompiledFunction g_origCpInside = nullptr;
    uint32_t g_cpSaveTest = 0;
    void cpInsideWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t ra = reg(ctx, 31);
        g_origCpInside(rdram, ctx, rt);
        if (g_cpSaveTest && g_frameNow >= g_cpSaveTest && ctx->pc == ra && ra >= 0x001C55E8u && ra < 0x001C5798u)
        {
            g_cpSaveTest = 0;
            setReg(ctx, 2, 1);
            std::cout << "[kz] coop: test checkpoint reached at frame " << g_frameNow << std::endl;
        }
    }

    // KZ_COOP_CPLOG=1: every CreatePlayer (ES FUN_001cc010(game, slot, ...)) with its caller and the slot's fields.
    PS2Runtime::RecompiledFunction g_origCreatePlayer = nullptr;
    // Player 1's spawn marker of the current level (t1 of its CreatePlayer): 'Spawn_Player' in 1.Trenches,
    // 'playerspawn' in 1.LightIndustry. Player 2 is added at the same marker (Lua KZSPAWN): with 'Spawn_Player' in a
    // level without it the spawn lookup failed and CreatePlayer crashed on the null actor (level 1 -> 2, 11-04).
    std::string g_p1Spawn; // player 1's spawn marker of the level being started (t1 of its CreatePlayer)
    void captureSpawn(uint8_t *rdram, uint32_t slot, uint32_t name)
    {
        if (slot != 0 || name < 0x100000u || name >= 0x02000000u)
            return;
        std::string n;
        for (int i = 0; i < 48 && rdram[(name + i) & 0x01FFFFFFu]; ++i)
        {
            const char c = static_cast<char>(rdram[(name + i) & 0x01FFFFFFu]);
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
                return;
            n += c;
        }
        if (n.empty())
            return;
        g_luaQueue.push_back("--q\nKZSPAWN = '" + n + "'\n");
        g_p1Spawn = n;
        std::cout << "[kz] coop: player 1 spawn marker '" << n << "'" << std::endl;
    }
    bool g_cpLog = false;
    void createPlayerLog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t game = reg(ctx, 4), slot = reg(ctx, 5), ra = reg(ctx, 31);
        captureSpawn(rdram, slot, reg(ctx, 9));
        if (!g_cpLog)
        {
            g_origCreatePlayer(rdram, ctx, rt);
            return;
        }
        const uint32_t gpi = game >= 0x100000u && game < 0x02000000u ? rd32(rdram, game + 0x74) : 0;
        const uint32_t slots = gpi ? rd32(rdram, gpi + 0x220) : 0;
        const uint32_t s = slots ? slots + slot * 0x1E0u : 0;
        char chr[24] = {};
        if (s)
            for (int i = 0; i < 23; ++i)
                chr[i] = static_cast<char>(rdram[(s + 0x40 + i) & 0x01FFFFFFu]);
        std::fprintf(stderr, "[kz] coop: CreatePlayer slot %u ra %08x n %d char '%s' prof %08x joy %d fn %02x actor %08x ctl %08x\n",
                     slot, ra, gpi ? static_cast<int>(rd32(rdram, gpi + 0x218)) : -1, chr, s ? rd32(rdram, s + 0x80) : 0,
                     s ? static_cast<int>(rd32(rdram, s + 0x84)) : 0, s ? rdram[(s + 0x89) & 0x01FFFFFFu] : 0,
                     s ? rd32(rdram, s + 0xD4) : 0, s ? rd32(rdram, s + 0xD8) : 0);
        auto str = [&](uint32_t a) {
            std::string out;
            if (a >= 0x100000u && a < 0x02000000u)
                for (int i = 0; i < 40 && rdram[(a + i) & 0x01FFFFFFu]; ++i)
                    out += static_cast<char>(rdram[(a + i) & 0x01FFFFFFu]);
            return out;
        };
        std::fprintf(stderr, "[kz] coop:   args a2 %08x '%s' a3 %08x '%s' t0 %08x '%s' t1 %08x\n", reg(ctx, 6), str(reg(ctx, 6)).c_str(),
                     reg(ctx, 7), str(reg(ctx, 7)).c_str(), reg(ctx, 8), str(reg(ctx, 8)).c_str(), reg(ctx, 9));
        {
            const uint32_t t1 = reg(ctx, 9);
            std::string names;
            if (t1 >= 0x100000u && t1 < 0x02000000u)
                for (uint32_t o = 0; o < 0x80; o += 4)
                {
                    const uint32_t v = rd32(rdram, t1 + o);
                    const std::string t = str(v);
                    if (t.size() >= 4)
                        names += " +" + std::to_string(o) + "='" + t + "'";
                }
            std::fprintf(stderr, "[kz] coop:   t1 words %08x %08x %08x %08x%s\n", t1 ? rd32(rdram, t1) : 0, t1 ? rd32(rdram, t1 + 4) : 0,
                         t1 ? rd32(rdram, t1 + 8) : 0, t1 ? rd32(rdram, t1 + 12) : 0, names.c_str());
        }
        g_origCreatePlayer(rdram, ctx, rt);
        if (ctx->pc == ra)
            std::fprintf(stderr, "[kz] coop: CreatePlayer slot %u -> %08x\n", slot, reg(ctx, 2));
    }

    // LOD of characters in co-op (Codex 11-04 lead): FUN_001f8d80 (per object with its own camera at +0x130) computes
    // the object's LOD from that camera's FOV (+0x70) and projection. With two players the camera still has the
    // split-screen values there (FOV x0.5, aspect x2: 32:9) -- fullScreenCameras restores them only before the draw
    // loop. Here the camera gets the full-screen values for the call and its own back after (KZ_COOP_LODCAM=0: off).
    PS2Runtime::RecompiledFunction g_origObjLod = nullptr;
    void objLodWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t obj = reg(ctx, 4), ra = reg(ctx, 31);
        uint32_t cam = obj >= 0x100000u && obj < 0x02000000u ? rd32(rdram, obj + 0x130) : 0;
        float fov = 0, asp = 0;
        if (cam >= 0x100000u && cam < 0x02000000u)
        {
            std::memcpy(&fov, rdram + cam + 0x70, 4);
            std::memcpy(&asp, rdram + cam + 0x74, 4);
            if (asp > 2.5f && fov > 1.0f && fov < 90.0f)
            {
                const float f2 = fov * 2.0f, a2 = asp * 0.5f;
                std::memcpy(rdram + cam + 0x70, &f2, 4);
                std::memcpy(rdram + cam + 0x74, &a2, 4);
            }
            else
                cam = 0;
        }
        else
            cam = 0;
        static int logN = 0;
        if (cam && std::getenv("KZ_COOP_LODCAM_LOG") && logN < 5)
        {
            ++logN;
            std::fprintf(stderr, "[kz] coop: object LOD camera %08x fov %.2f aspect %.2f -> full screen\n", cam, fov, asp);
        }
        // experiment (KZ_COOP_LODCOUNT1=1): the player count reads 1 during the call
        static const bool count1 = std::getenv("KZ_COOP_LODCOUNT1") != nullptr;
        uint32_t gpiC = 0, savedN = 0;
        if (count1)
        {
            const uint32_t g = rd32(rdram, kzAddrs().gameObjectPtr);
            gpiC = g >= 0x100000u && g < 0x02000000u ? rd32(rdram, g + 0x74) : 0;
            if (gpiC >= 0x100000u && gpiC < 0x02000000u && rd32(rdram, gpiC + 0x218) == 2u)
            {
                savedN = 2;
                wr32(rdram, gpiC + 0x218, 1);
            }
        }
        g_origObjLod(rdram, ctx, rt);
        if (savedN)
            wr32(rdram, gpiC + 0x218, savedN);
        if (cam && ctx->pc == ra)
        {
            std::memcpy(rdram + cam + 0x70, &fov, 4);
            std::memcpy(rdram + cam + 0x74, &asp, 4);
        }
    }

    // ---- Native player 2 creation (kzCoopNative). The engine runs a script function by name with FUN_002e3890(script
    // wrapper, name) (wrapper +0x14 = lua_State; FUN_002e3928). Right after the mission script's "OnInitialize" (the
    // level's AddPlayer(spawn) for player 0) the same library function creates player 2: AddPlayer(spawn, 1) in the same
    // state, at the same moment as player 1, before the level-start checkpoint. The Lua runs through lua_dobuffer with its
    // return address set to FUN_002e3890 itself: if the guest yields meanwhile, the dispatcher comes back into this hook
    // (ra == own entry) and the saved registers are restored there (same trick as the frame limiter's startLua).
    PS2Runtime::RecompiledFunction g_origCallByName = nullptr;
    constexpr uint32_t kCallByName = 0x002E3890u;
    // Phases: 1 = the mission's OnInitialize runs with its return address set to kCallByName; 2 = our Lua runs, also
    // returning to kCallByName. Both may yield to other guest threads: the return then comes back through the
    // dispatcher into this hook with pc == ra == kCallByName (no real call ever has ra == the callee's entry).
    struct InitSaved
    {
        int phase = 0;
        uint32_t ra = 0, sp = 0, L = 0;
    } g_init;
    void initStartLua(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const std::string spawn = g_p1Spawn.empty() ? std::string("Spawn_Player") : g_p1Spawn;
        const std::string code =
            "KZOUT = nil\nKZSPAWN = '" + spawn + "'\n" + kFixDef +
            "call(function()\n"
            "  KZP1 = PLAYER\n"
            "  KZP2 = KZADDP2(KZSPAWN, 1)\n"
            "  KZOUT = 'created at OnInitialize as ' .. tostring(gGamePlayInfo:GetPlayerCharacter(1)) .. ' on ' .. KZSPAWN\n"
            "end, {}, 'x', function(e) KZOUT = 'ERROR ' .. tostring(e) end)\n" + kSetup;
        // lua_dobuffer(L, code, len, "=kz") with the code on the guest stack below the caller's frame
        const uint32_t sp = g_init.sp;
        const uint32_t len = static_cast<uint32_t>(code.size());
        const uint32_t area = (len + 16u + 15u) & ~15u;
        const uint32_t buf = sp - area - 0x40u;
        const uint32_t nm = buf + ((len + 4u) & ~3u);
        std::memcpy(rdram + (buf & kRamMask), code.data(), len);
        rdram[(buf + len) & kRamMask] = 0;
        std::memcpy(rdram + (nm & kRamMask), "=kz", 4);
        g_init.phase = 2;
        setReg(ctx, 29, (buf - 0x100u) & ~15u);
        setReg(ctx, 4, g_init.L);
        setReg(ctx, 5, buf);
        setReg(ctx, 6, len);
        setReg(ctx, 7, nm);
        setReg(ctx, 31, kCallByName);
        ctx->pc = luaAddrs().doBuffer;
        g_doBuffer(rdram, ctx, rt);
    }
    // Returns true when the guest is back in the engine (ctx->pc = its continuation) or yielded.
    void initStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        while (ctx->pc == kCallByName && reg(ctx, 31) == kCallByName && g_init.phase)
        {
            if (g_init.phase == 1)
            {
                initStartLua(rdram, ctx, rt); // OnInitialize is over: player 1 exists
                continue;
            }
            // phase 2: our Lua is over
            const int result = static_cast<int>(reg(ctx, 2));
            g_init.phase = 0;
            setReg(ctx, 29, g_init.sp);
            setReg(ctx, 31, g_init.ra);
            ctx->pc = g_init.ra;
            uint32_t tag = 0, value = 0;
            std::string out;
            if (luaGlobal(rdram, g_init.L, "KZOUT", tag, value) && tag == 3u)
                out = cstr(rdram, value + 0x14, 4000);
            std::cout << "[kz] coop: native player 2 (lua result " << result << ") " << out << std::endl;
            return;
        }
    }
    void callByNameWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        if (g_init.phase && ctx->pc == kCallByName && reg(ctx, 31) == kCallByName)
        {
            initStep(rdram, ctx, rt); // a phase finished after yielding
            return;
        }
        const uint32_t wrapper = reg(ctx, 4), name = reg(ctx, 5), ra = reg(ctx, 31);
        static const bool logCalls = std::getenv("KZ_COOP_CALLLOG") != nullptr;
        if (logCalls && name >= 0x100000u && name < 0x02000000u)
            std::cout << "[kz] coop: script call '" << cstr(rdram, name, 32) << "' wrapper " << std::hex << wrapper << " ra " << ra << std::dec << std::endl;
        bool mine = false;
        if (kzCoopNative() && kzNetHasPlayer2() && !g_init.phase && g_doBuffer && name >= 0x100000u && name < 0x02000000u &&
            wrapper >= 0x100000u && wrapper < 0x02000000u && cstr(rdram, name, 16) == "OnInitialize")
        {
            const uint32_t L = rd32(rdram, wrapper + 0x14);
            uint32_t tag = 0, value = 0;
            // the mission script (the level library defines AddPlayer), not the game mode script
            if (L >= 0x100000u && L < 0x02000000u && luaGlobal(rdram, L, "AddPlayer", tag, value) && kzCoopSlot(rdram, 1))
            {
                mine = true;
                g_init.phase = 1;
                g_init.ra = ra;
                g_init.sp = reg(ctx, 29);
                g_init.L = L;
                setReg(ctx, 31, kCallByName);
            }
        }
        g_origCallByName(rdram, ctx, rt);
        if (mine)
            initStep(rdram, ctx, rt); // OnInitialize returned without yielding
    }

    // ---- PathResource guard + watch (11-05 evening, ES addresses, Ghidra). Santi's full level 1 -> "continue" crashed
    // both copies at level unload: FUN_001bdcf8 -> FUN_0017c7f0 (remove every world entity) -> release -> PathResource
    // destructor FUN_0048ffc0 (vtable 0x52CCE8, class "PathResource", "Samples" = PathElement[count +0x10] of 0x30 bytes
    // at +0x18) -> element vtable +0x14 -> jump into data. The object was still alive (base dtor FUN_0017f180 would have
    // set vtable 0x521798) but from element 101 on its sample array held 0x20-byte records of someone else: the array
    // memory had been freed/reused during play. Guard: if any element is not a valid object, the array is not ours any
    // more -> do not destroy nor free it (count/pointer zeroed, the base destructor still runs). Watch: log who frees or
    // allocates memory inside a live PathResource's array (KZ_PATH_WATCH=0 turns it off).
    constexpr uint32_t kPathCtor = 0x0048FF60u, kPathDtor = 0x0048FFC0u, kPathVtbl = 0x0052CCE8u;
    constexpr uint32_t kFree = 0x00172DA8u, kMalloc = 0x001727D0u;
    PS2Runtime::RecompiledFunction g_origPathCtor = nullptr, g_origPathDtor = nullptr, g_origFree = nullptr,
                                   g_origMalloc = nullptr;
    std::vector<uint32_t> g_paths;
    uint32_t g_pathInDtor = 0;
    int g_pathLogs = 0;
    void pathStack(const uint8_t *rdram, const R5900Context *ctx)
    {
        const uint32_t sp = reg(ctx, 29);
        std::fprintf(stderr, "[kz] path:   ra %08x stack", reg(ctx, 31));
        int n = 0;
        for (uint32_t o = 0; o < 0x600u && n < 20; o += 4)
        {
            const uint32_t w = rd32(rdram, sp + o);
            if (w >= 0x00100000u && w < 0x00520000u && !(w & 3u))
            {
                std::fprintf(stderr, " +%x=%08x", o, w);
                ++n;
            }
        }
        std::fprintf(stderr, "\n");
    }
    // address inside the sample array of a live PathResource -> that object, else 0
    uint32_t pathOwner(const uint8_t *rdram, uint32_t p, uint32_t &arr, uint32_t &count)
    {
        for (uint32_t obj : g_paths)
        {
            if (obj == g_pathInDtor || rd32(rdram, obj) != kPathVtbl)
                continue;
            arr = rd32(rdram, obj + 0x18);
            count = rd32(rdram, obj + 0x10);
            if (arr && count < 0x10000u && p >= arr && p < arr + count * 0x30u)
                return obj;
        }
        return 0;
    }
    void pathCtorWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t obj = reg(ctx, 4);
        g_origPathCtor(rdram, ctx, rt);
        if (std::find(g_paths.begin(), g_paths.end(), obj) == g_paths.end())
            g_paths.push_back(obj);
    }
    void pathDtorWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t obj = reg(ctx, 4);
        const uint32_t count = rd32(rdram, obj + 0x10), arr = rd32(rdram, obj + 0x18);
        static const bool test = std::getenv("KZ_PATH_TEST") != nullptr; // reproduces Santi's crash (element 101)
        if (test && arr && count > 101)
            wr32(rdram, arr + 101 * 0x30u, 0x31100000u);
        int bad = -1;
        for (uint32_t i = 0; arr && i < count && i < 0x10000u; ++i)
        {
            const uint32_t v = rd32(rdram, arr + i * 0x30u);
            const uint32_t fn = (v >= 0x00500000u && v < 0x00560000u && !(v & 3u)) ? rd32(rdram, v + 0x14) : 0;
            if (!fn || !rt->hasFunction(fn))
            {
                bad = static_cast<int>(i);
                break;
            }
        }
        if (bad >= 0)
        {
            std::fprintf(stderr, "[kz] path: PathResource %08x has a reused sample array %08x (element %d of %u is "
                                 "not an object): leaving the array alone\n", obj, arr, bad, count);
            pathStack(rdram, ctx);
            wr32(rdram, obj + 0x10, 0);
            wr32(rdram, obj + 0x18, 0);
        }
        const bool tracked = std::find(g_paths.begin(), g_paths.end(), obj) != g_paths.end();
        std::fprintf(stderr, "[kz] path: destroy PathResource %08x (%u samples at %08x, %s)\n", obj, count, arr,
                     tracked ? "watched" : "not watched");
        g_paths.erase(std::remove(g_paths.begin(), g_paths.end(), obj), g_paths.end());
        const uint32_t prev = g_pathInDtor;
        g_pathInDtor = obj;
        g_origPathDtor(rdram, ctx, rt);
        g_pathInDtor = prev;
    }
    void freeWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t p = reg(ctx, 4);
        uint32_t arr = 0, count = 0;
        if (p && !g_paths.empty() && g_pathLogs < 40)
            if (const uint32_t obj = pathOwner(rdram, p, arr, count))
            {
                ++g_pathLogs;
                std::fprintf(stderr, "[kz] path: WATCH free(%08x) inside the live sample array %08x (+%x, %u elements) "
                                     "of PathResource %08x\n", p, arr, p - arr, count, obj);
                pathStack(rdram, ctx);
            }
        g_origFree(rdram, ctx, rt);
    }
    void mallocWrap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t ra = reg(ctx, 31), sp = reg(ctx, 29), a0 = reg(ctx, 4), a1 = reg(ctx, 5);
        g_origMalloc(rdram, ctx, rt);
        const uint32_t p = reg(ctx, 2);
        uint32_t arr = 0, count = 0;
        if (p && !g_paths.empty() && g_pathLogs < 40)
            if (const uint32_t obj = pathOwner(rdram, p, arr, count))
            {
                ++g_pathLogs;
                std::fprintf(stderr, "[kz] path: WATCH malloc(a0 %x a1 %x) = %08x inside the live sample array %08x "
                                     "(+%x, %u elements) of PathResource %08x\n", a0, a1, p, arr, p - arr, count, obj);
                R5900Context c = *ctx;
                setReg(&c, 29, sp);
                setReg(&c, 31, ra);
                pathStack(rdram, &c);
            }
    }
    void pathGuardInstall(PS2Runtime &runtime)
    {
        if (!kzIsEs() || (std::getenv("KZ_PATH_GUARD") && *std::getenv("KZ_PATH_GUARD") == '0'))
            return;
        g_origPathCtor = runtime.lookupFunction(kPathCtor);
        g_origPathDtor = runtime.lookupFunction(kPathDtor);
        if (!g_origPathCtor || !g_origPathDtor || !runtime.replaceFunction(kPathCtor, &pathCtorWrap) ||
            !runtime.replaceFunction(kPathDtor, &pathDtorWrap))
        {
            std::cerr << "[kz] path: PathResource guard hooks failed" << std::endl;
            return;
        }
        if (std::getenv("KZ_PATH_WATCH") && *std::getenv("KZ_PATH_WATCH") == '0')
            return;
        g_origFree = runtime.lookupFunction(kFree);
        g_origMalloc = runtime.lookupFunction(kMalloc);
        if (!g_origFree || !g_origMalloc || !runtime.replaceFunction(kFree, &freeWrap) ||
            !runtime.replaceFunction(kMalloc, &mallocWrap))
            std::cerr << "[kz] path: allocator watch hooks failed" << std::endl;
    }

    void p2ConfigInstall(PS2Runtime &runtime)
    {
        pathGuardInstall(runtime);
        if (kzCoopNative() && kzIsEs())
        {
            g_origCallByName = runtime.hasFunction(kCallByName) ? runtime.lookupFunction(kCallByName) : nullptr;
            if (!g_origCallByName || !runtime.replaceFunction(kCallByName, &callByNameWrap))
                std::cerr << "[kz] coop: OnInitialize hook failed" << std::endl;
        }
        if (std::getenv("KZ_COOP_LODCAM") && *std::getenv("KZ_COOP_LODCAM") == '1') // experiment, no visible effect (11-05)
        {
            g_origObjLod = runtime.lookupFunction(0x001F8D80u);
            if (!g_origObjLod || !runtime.replaceFunction(0x001F8D80u, &objLodWrap))
                std::cerr << "[kz] coop: object LOD camera hook failed" << std::endl;
        }
        g_cpLog = std::getenv("KZ_COOP_CPLOG") != nullptr;
        {
            g_origCreatePlayer = runtime.lookupFunction(0x001CC010u);
            if (!g_origCreatePlayer || !runtime.replaceFunction(0x001CC010u, &createPlayerLog))
                std::cerr << "[kz] coop: CreatePlayer log hook failed" << std::endl;
        }
        if (!kzIsEs() || std::getenv("KZ_COOP_NOCONFIG"))
            return;
        g_origCpRestore = runtime.lookupFunction(0x001C5F08u);
        g_origCpSave = runtime.lookupFunction(0x001C5798u);
        g_setSlotPlayer = runtime.lookupFunction(0x001B9E00u);
        g_origHudMsg = runtime.lookupFunction(0x001BB668u);
        if (!g_origHudMsg || !runtime.replaceFunction(0x001BB668u, &hudMsgWrap))
            std::cerr << "[kz] coop: on-screen message hook failed" << std::endl;
        if (!g_origCpRestore || !g_origCpSave || !runtime.replaceFunction(0x001C5F08u, &cpRestoreWrap) ||
            !runtime.replaceFunction(0x001C5798u, &cpSaveWrap))
            std::cerr << "[kz] coop: checkpoint hooks failed" << std::endl;
        if (const char *e = std::getenv("KZ_COOP_CPSAVE_TEST"))
        {
            g_cpSaveTest = static_cast<uint32_t>(std::atoi(e));
            g_origCpInside = runtime.lookupFunction(0x00272160u);
            if (!g_origCpInside || !runtime.replaceFunction(0x00272160u, &cpInsideWrap))
                std::cerr << "[kz] coop: checkpoint test hook failed" << std::endl;
        }
        g_origSetNr = runtime.lookupFunction(0x001B92F0u);
        g_origSetChar = runtime.lookupFunction(0x001B9AA8u);
        g_origSetLevel = runtime.lookupFunction(0x001B8AD8u);
        if (!g_origSetNr || !g_origSetChar || !g_origSetLevel || !runtime.replaceFunction(0x001B92F0u, &setNrWrap) ||
            !runtime.replaceFunction(0x001B9AA8u, &setCharWrap) || !runtime.replaceFunction(0x001B8AD8u, &setLevelWrap))
            std::cerr << "[kz] coop: player 2 configuration hooks failed" << std::endl;
    }
}

void kzCoopEnsureTwo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
{
    const uint32_t g = rd32(rdram, kzAddrs().gameObjectPtr);
    if (g < 0x100000u || g >= 0x02000000u)
        return;
    const uint32_t gpi = rd32(rdram, g + 0x74);
    if (gpi >= 0x100000u && gpi < 0x02000000u && static_cast<int>(rd32(rdram, gpi + 0x218)) < 2)
    {
        configureP2(rdram, ctx, rt, gpi);
        std::cout << "[kz] coop: two players configured again" << std::endl;
    }
}

void kzCoopArgLogInstall(PS2Runtime &runtime);
void kzCoopInstall(PS2Runtime &runtime)
{
    p2ConfigInstall(runtime);
    parseScript();
    kzCoopArgLogInstall(runtime);
    kzCoopViewInstall(runtime);
    const LuaAddrs a = luaAddrs();
    if (a.doBuffer)
        g_doBuffer = runtime.lookupFunction(a.doBuffer);
}

// ---- 11-05: dump of every Lua chunk the game loads (KZ_LUA_DUMP=<dir>). The game's scripts are precompiled Lua 4
// resources read from a stream (ES FUN_003a8898 -> FUN_002e3c90 -> undump FUN_002e4ec0, every read through LoadBlock
// FUN_002e44f8); hand edits in generated_es call these. Files: <dir>\chunk_NNNN.luac (ESC + the bytes undump read).
#include <filesystem>
#include <fstream>
namespace
{
    std::vector<uint8_t> g_luaDumpBuf;
    bool g_luaDumpOn = false;
    int g_luaDumpN = 0;
    const char *luaDumpDir()
    {
        static const char *d = std::getenv("KZ_LUA_DUMP");
        return d && *d ? d : nullptr;
    }
}
void kzLuaDumpBegin()
{
    if (!luaDumpDir())
        return;
    g_luaDumpBuf.clear(); // every byte comes from LoadBlock FUN_002e44f8 or the byte read FUN_00146b88
    g_luaDumpOn = true;
}
void kzLuaDumpBytes(const uint8_t *rdram, uint32_t addr, uint32_t n)
{
    if (!g_luaDumpOn || n > 0x1000000u)
        return;
    for (uint32_t i = 0; i < n; ++i)
        g_luaDumpBuf.push_back(rdram[(addr + i) & 0x01FFFFFFu]);
}
void kzLuaDumpEnd()
{
    if (!g_luaDumpOn)
        return;
    g_luaDumpOn = false;
    std::filesystem::create_directories(luaDumpDir());
    char name[64];
    std::snprintf(name, sizeof(name), "chunk_%04d.luac", g_luaDumpN++);
    std::ofstream(std::filesystem::path(luaDumpDir()) / name, std::ios::binary)
        .write(reinterpret_cast<const char *>(g_luaDumpBuf.data()), static_cast<std::streamsize>(g_luaDumpBuf.size()));
    std::printf("[kz] lua dump: %s (%zu bytes)\n", name, g_luaDumpBuf.size());
}
