/*
 * Which buffer is which swapchain slot, for games that give every slot its
 * own nvmap object (Smash Ultimate: slots 0, 1, 2 registered as three ids,
 * each at offset 0). The parcels give no addresses, so the slots are matched
 * to the candidate buffers (every device-mapped region of the buffer's size)
 * by watching them change while the game presents.
 *
 * One sample per finished present: a signature of every candidate, taken
 * when the present of slot s is finished. Between two consecutive presents
 * the GPU finished drawing the later one's slot, while the earlier one's slot
 * stayed on screen untouched - so slot s's buffer changes in every interval
 * that ends at slot s and in (almost) no other. Each candidate scores, per
 * slot, its change rate in the intervals ending at that slot minus its mean
 * rate in the intervals ending at the others: slot s's buffer scores ~1000, a
 * render target of the same size that changes every frame ~0, a buffer that
 * never changes 0. Intervals where nothing changed (a still picture) and
 * intervals spanning more than one present are not counted.
 *
 * No hardware in here, so it has a host test (test/slotmatch_test.cpp).
 */
#pragma once
#include <cstdint>
#include <cstring>

namespace ams::mitm::applet::slotmatch {

    constexpr uint32_t MaxSlots = 8;
    /* v0.7.3: 48 (was 24) - Minecraft back in handheld after docked had
     * more than 24 regions of a 720p buffer's size, its new buffers past them */
    constexpr uint32_t MaxCand  = 48;

    constexpr uint32_t MinPerSlot = 8;      /* counted intervals per slot before any match */
    constexpr int32_t  MinScore   = 600;    /* of 1000 */
    constexpr int32_t  MinMargin  = 300;    /* over the slot's second-best candidate */
    constexpr uint32_t GiveUpAfter = 900;   /* counted intervals (~15 s of moving picture at 60 fps) */
    constexpr uint32_t MaxSamples  = 3600;  /* samples of any kind (~1 min): a still picture stops it too */

    struct Matcher {
        uint32_t nslots = 0, ncand = 0;
        uint32_t sig[MaxCand] = {};
        uint32_t last = 0;                  /* the present count sig[] was taken at */
        bool have = false;
        uint16_t hit[MaxSlots][MaxCand] = {};
        uint16_t tot[MaxSlots] = {};
        uint32_t intervals = 0;
        uint32_t samples = 0;

        void Reset(uint32_t slots, uint32_t cands) {
            nslots = slots < MaxSlots ? slots : MaxSlots;
            ncand  = cands < MaxCand ? cands : MaxCand;
            have = false;
            last = 0;
            intervals = 0;
            samples = 0;
            std::memset(sig, 0, sizeof(sig));
            std::memset(hit, 0, sizeof(hit));
            std::memset(tot, 0, sizeof(tot));
        }

        /* the candidates' signatures once present `count` (of `slot`) is finished */
        void Add(uint32_t count, uint32_t slot, const uint32_t *sigs) {
            if (slot >= nslots) { return; }
            ++samples;
            if (have && count == last + 1 && tot[slot] < 0xFFFF) {
                bool any = false;
                for (uint32_t c = 0; c < ncand; ++c) { any |= sigs[c] != sig[c]; }
                if (any) {
                    ++tot[slot];
                    ++intervals;
                    for (uint32_t c = 0; c < ncand; ++c) { if (sigs[c] != sig[c]) { ++hit[slot][c]; } }
                }
            }
            std::memcpy(sig, sigs, ncand * sizeof(uint32_t));
            last = count;
            have = true;
        }

        /* true once every slot has a candidate that stands out, each a
         * different one: out[slot] = candidate, *weakest = the lowest score */
        bool Match(uint8_t *out, int32_t *weakest) const {
            if (nslots < 2) { return false; }
            for (uint32_t s = 0; s < nslots; ++s) { if (tot[s] < MinPerSlot) { return false; } }
            uint8_t m[MaxSlots] = {};
            uint64_t used = 0;
            int32_t worst = 1000;
            for (uint32_t s = 0; s < nslots; ++s) {
                int32_t best = -1, best_score = -100000, second = -100000;
                for (uint32_t c = 0; c < ncand; ++c) {
                    const int32_t rate = static_cast<int32_t>(hit[s][c] * 1000u / tot[s]);
                    int32_t other = 0;
                    for (uint32_t s2 = 0; s2 < nslots; ++s2) {
                        if (s2 != s) { other += static_cast<int32_t>(hit[s2][c] * 1000u / tot[s2]); }
                    }
                    const int32_t score = rate - other / static_cast<int32_t>(nslots - 1);
                    if (score > best_score) { second = best_score; best_score = score; best = static_cast<int32_t>(c); }
                    else if (score > second) { second = score; }
                }
                if (best < 0 || best_score < MinScore || best_score - second < MinMargin || (used & (UINT64_C(1) << best)) != 0) { return false; }
                m[s] = static_cast<uint8_t>(best);
                used |= UINT64_C(1) << best;
                if (best_score < worst) { worst = best_score; }
            }
            std::memcpy(out, m, nslots);
            if (weakest != nullptr) { *weakest = worst; }
            return true;
        }

        bool GaveUp() const { return intervals >= GiveUpAfter || samples >= MaxSamples; }
    };

    /* A first guess while the picture stands still and Match has nothing to
     * go on (DuckStation's menu): a still picture is the same in every slot,
     * and no texture memory holds the same few megabytes twice. Out: nslots
     * candidates whose signatures are equal and whose samples are not one
     * uniform value (a cleared buffer), the first such set. `grouped`: the
     * slots are offsets of one object, so a set is nslots consecutive
     * candidates (FindSlotCandidates lists one region's slots together). */
    inline bool StillGuess(const uint32_t *sigs, const bool *varied, uint32_t ncand, uint32_t nslots, bool grouped, uint8_t *out) {
        if (nslots < 2 || nslots > MaxSlots || ncand < nslots) { return false; }
        if (grouped) {
            for (uint32_t c = 0; c + nslots <= ncand; c += nslots) {
                bool same = varied[c];
                for (uint32_t k = 1; k < nslots && same; ++k) { same = varied[c + k] && sigs[c + k] == sigs[c]; }
                if (same) { for (uint32_t k = 0; k < nslots; ++k) { out[k] = static_cast<uint8_t>(c + k); } return true; }
            }
            return false;
        }
        for (uint32_t c = 0; c < ncand; ++c) {
            if (!varied[c]) { continue; }
            uint32_t n = 0;
            for (uint32_t d = c; d < ncand && n < nslots; ++d) {
                if (varied[d] && sigs[d] == sigs[c]) { out[n++] = static_cast<uint8_t>(d); }
            }
            if (n == nslots) { return true; }
        }
        return false;
    }

}
