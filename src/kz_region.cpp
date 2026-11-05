#include "kz_region.h"

#include "kz_addrs.h"
#include "kz_pal_table.h"

#include "ps2_runtime.h"

#include <cstring>
#include <iostream>
#include <sstream>

namespace
{
    KzRegion g_region = KzRegion::Us;
    bool g_selected = false;

    bool tableComplete(const KzAddrs &a)
    {
        static_assert(sizeof(KzAddrs) % sizeof(uint32_t) == 0, "KzAddrs holds only uint32_t fields");
        uint32_t w[sizeof(KzAddrs) / sizeof(uint32_t)];
        std::memcpy(w, &a, sizeof(w));
        for (uint32_t v : w)
            if (v == 0u)
                return false;
        return true;
    }
}

KzRegion kzRegion() { return g_region; }

const char *kzRegionTitle(KzRegion r)
{
    return r == KzRegion::Pal ? "Killzone (Europe)" : r == KzRegion::Es ? "Killzone (Europe, Multi-6)" : "Killzone (USA)";
}
const char *kzRegionSerial(KzRegion r) { return r == KzRegion::Pal ? "SCES-52893" : r == KzRegion::Es ? "SCES-52004" : "SCUS-97402"; }
const char *kzRegionBootElf(KzRegion r) { return r == KzRegion::Pal ? "SCES_528.93" : r == KzRegion::Es ? "SCES_520.04" : "SCUS_974.02"; }
const char *kzRegionBootFolder(KzRegion r) { return r == KzRegion::Pal ? "disc_pal" : r == KzRegion::Es ? "disc_es" : "disc"; }
uint32_t kzRegionBootElfXorCrc(KzRegion r) { return r == KzRegion::Pal ? 0x69FE9BB6u : r == KzRegion::Es ? 0x6624A78Cu : 0xCAAEC49Cu; }

bool kzRegionSupported(KzRegion r)
{
    if (r == KzRegion::Us)
        return true;
#if defined(KZ_HAVE_PAL)
    if (r == KzRegion::Pal)
        return true;
#endif
#if defined(KZ_HAVE_ES)
    if (r == KzRegion::Es)
        return true;
#endif
    return false;
}

bool kzSelectRegion(KzRegion r, std::string *error)
{
    auto fail = [error](const std::string &msg) {
        if (error)
            *error = msg;
        std::cerr << "[kz] " << msg << std::endl;
        return false;
    };
    if (g_selected)
        return r == g_region ? true : fail("the game region is already chosen; it cannot change while running");
    if (!kzRegionSupported(r))
        return fail(std::string(kzRegionTitle(r)) + " (" + kzRegionSerial(r) + ") is not supported by this build (built without the code for that disc).");
    const KzAddrs &addrs = r == KzRegion::Pal ? kKzAddrsPal : r == KzRegion::Es ? kKzAddrsEs : kKzAddrsUs;
    if (!tableComplete(addrs))
        return fail(std::string("internal error: the guest address table for ") + kzRegionSerial(r) + " is incomplete");
#if defined(KZ_HAVE_PAL)
    if (r == KzRegion::Pal)
    {
        // The live table is the one the US generated code defines (g_ps2RecompiledFunctionTable, Base, SlotCount; every
        // guest call indexes it). It must reach past the last PAL entry; then the PAL code replaces every slot.
        const uint32_t base = g_ps2RecompiledFunctionTableBase;
        const uint32_t slots = g_ps2RecompiledFunctionTableSlotCount;
        const uint32_t palEnd = kzPalRegisterFunctionTableEnd;
        if (palEnd < base || ((palEnd - base) >> 2) > slots || palEnd > g_ps2RecompiledFunctionTableEnd)
        {
            std::ostringstream m;
            m << "internal error: the function table (0x" << std::hex << base << "..0x" << g_ps2RecompiledFunctionTableEnd
              << ", " << std::dec << slots << " slots) does not cover the PAL code (end 0x" << std::hex << palEnd
              << "); the US code must be generated with a table that covers it";
            return fail(m.str());
        }
        kzPalRegisterFunctionTable(g_ps2RecompiledFunctionTable, base, slots);
        std::cout << "[kz] function table: PAL code installed (" << ((palEnd - base) >> 2) << " of " << slots << " slots)" << std::endl;
    }
#endif
#if defined(KZ_HAVE_ES)
    if (r == KzRegion::Es)
    {
        const uint32_t base = g_ps2RecompiledFunctionTableBase;
        const uint32_t slots = g_ps2RecompiledFunctionTableSlotCount;
        const uint32_t esEnd = kzEsRegisterFunctionTableEnd;
        if (esEnd < base || ((esEnd - base) >> 2) > slots || esEnd > g_ps2RecompiledFunctionTableEnd)
        {
            std::ostringstream m;
            m << "internal error: the function table (0x" << std::hex << base << "..0x" << g_ps2RecompiledFunctionTableEnd
              << ") does not cover the SCES-52004 code (end 0x" << esEnd << "); raise table_end_min in config/killzone.toml";
            return fail(m.str());
        }
        kzEsRegisterFunctionTable(g_ps2RecompiledFunctionTable, base, slots);
        std::cout << "[kz] function table: SCES-52004 code installed (" << ((esEnd - base) >> 2) << " of " << slots << " slots)" << std::endl;
    }
#endif
    g_region = r;
    kz_detail::g_addrs = &addrs;
    g_selected = true;
    std::cout << "[kz] game: " << kzRegionTitle(r) << " - " << kzRegionSerial(r) << " (" << kzRegionBootElf(r) << ")" << std::endl;
    return true;
}
