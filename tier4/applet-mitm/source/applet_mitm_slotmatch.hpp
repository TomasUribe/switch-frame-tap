/*
 * Which buffer is which swapchain slot, for games that give every slot its
 * own nvmap object (Smash Ultimate: slots 0, 1, 2 registered as three ids,
 * each at offset 0). The parcels give no addresses, so the slots are matched
 * to the candidate buffers (every device-mapped region of the buffer's size)
 * by watching them change while the game presents.
 *
 * One sample per finished present: a signature of every candidate, taken
 * when a present is finished. Between two samples the GPU finished drawing
 * the slots of the presents in between, while the other slots stayed on
 * screen untouched - so slot s's buffer changes in every interval that drew
 * slot s and in (almost) no other. Each candidate scores, per slot, its change
 * rate in the intervals that drew the slot minus its rate in the intervals
 * that did not: slot s's buffer scores ~1000, a render target of the same
 * size that changes every frame ~0, a buffer that never changes 0.
 * Intervals where nothing changed (a still picture), and intervals that drew
 * every slot or whose slots are unknown, are not counted.
 *
 * v0.7.9: intervals may span several presents (their slots from the present
 * ring). Minecraft with Vibrant Visuals: the stream took 9 of 21 presents a
 * second, so two consecutive samples were rare and 43 same-size render
 * targets never got told apart (the viewer showed one of them).
 *
 * No hardware in here, so it has a host test (test/slotmatch_test.cpp).
 */
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace ams::mitm::applet::slotmatch {

    constexpr uint32_t MaxSlots = 8;
    /* v0.7.3: 48 (was 24) - Minecraft back in handheld after docked had
     * more than 24 regions of a 720p buffer's size, its new buffers past them.
     * v0.7.23: 96 - Maestro (#70) has 95 buffers of its 720p slots' size, none
     * mapped for the display; the slots were past the 48 the list kept, the
     * match never came and the stream showed a render target (violet noise).
     * The stream samples fewer points per buffer past 48 (SigPointsMany), so
     * a sample costs what it did. */
    constexpr uint32_t MaxCand  = 96;

    constexpr uint32_t MinPerSlot = 8;      /* counted intervals per slot before any match */
    constexpr int32_t  MinScore   = 600;    /* of 1000 */
    constexpr int32_t  MinMargin  = 300;    /* over the slot's second-best candidate */
    constexpr uint32_t GiveUpAfter = 900;   /* counted intervals (~15 s of moving picture at 60 fps) */
    /* v0.7.24: counted intervals without a slot drawn once - the game does not
     * use it. Maestro (#70) draws slots 0 and 1 of its three, every interval
     * (454 drawn / 446 not for each): both stood out (896 and 898), but the
     * match waited for slot 2 and never came. */
    constexpr uint32_t UnusedAfter = 48;
    constexpr uint32_t MaxSamples  = 3600;  /* samples of any kind (~1 min): a still picture stops it too */

    struct Matcher {
        uint32_t nslots = 0, ncand = 0;
        uint32_t sig[MaxCand] = {};
        uint32_t last = 0;                  /* the present count sig[] was taken at */
        bool have = false;
        uint16_t hit_in[MaxSlots][MaxCand] = {};    /* changed, in intervals that drew the slot */
        uint16_t hit_out[MaxSlots][MaxCand] = {};   /* changed, in intervals that did not */
        uint16_t tot_in[MaxSlots] = {};
        uint16_t tot_out[MaxSlots] = {};
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
            std::memset(hit_in, 0, sizeof(hit_in));
            std::memset(hit_out, 0, sizeof(hit_out));
            std::memset(tot_in, 0, sizeof(tot_in));
            std::memset(tot_out, 0, sizeof(tot_out));
        }

        /* the candidates' signatures once present `count` is finished; `drawn`:
         * a bit per slot drawn by the presents since the last sample (last+1 ..
         * count), 0 when unknown */
        void AddSpan(uint32_t count, uint32_t drawn, const uint32_t *sigs) {
            ++samples;
            const uint32_t all = (nslots >= 32) ? 0xFFFFFFFFu : ((1u << nslots) - 1);
            drawn &= all;
            if (have && count > last && drawn != 0 && drawn != all) {
                bool any = false;
                for (uint32_t c = 0; c < ncand; ++c) { any |= sigs[c] != sig[c]; }
                if (any) {
                    ++intervals;
                    for (uint32_t s = 0; s < nslots; ++s) {
                        const bool in = (drawn >> s) & 1;
                        uint16_t &tot = in ? tot_in[s] : tot_out[s];
                        uint16_t *hit = in ? hit_in[s] : hit_out[s];
                        if (tot == 0xFFFF) { continue; }
                        ++tot;
                        for (uint32_t c = 0; c < ncand; ++c) { if (sigs[c] != sig[c]) { ++hit[c]; } }
                    }
                }
            }
            std::memcpy(sig, sigs, ncand * sizeof(uint32_t));
            last = count;
            have = true;
        }

        /* one present after the last sample: it drew `slot` */
        void Add(uint32_t count, uint32_t slot, const uint32_t *sigs) {
            AddSpan(count, (have && count == last + 1 && slot < nslots) ? (1u << slot) : 0, sigs);
        }

        /* true once every slot has a candidate that stands out, each a
         * different one: out[slot] = candidate, *weakest = the lowest score */
        bool Match(uint8_t *out, int32_t *weakest, uint32_t *unused_out = nullptr) const {
            if (nslots < 2) { return false; }
            uint32_t unused = 0, live = 0;
            for (uint32_t s = 0; s < nslots; ++s) {
                if (tot_in[s] == 0 && tot_out[s] >= UnusedAfter) { unused |= 1u << s; continue; }
                if (tot_in[s] < MinPerSlot || tot_out[s] < MinPerSlot) { return false; }
                ++live;
            }
            if (live == 0) { return false; }
            uint8_t m[MaxSlots] = {};
            uint64_t used[(MaxCand + 63) / 64] = {};
            int32_t worst = 1000;
            for (uint32_t s = 0; s < nslots; ++s) {
                if ((unused >> s) & 1) { continue; }
                int32_t best = -1, best_score = -100000, second = -100000;
                for (uint32_t c = 0; c < ncand; ++c) {
                    const int32_t score = static_cast<int32_t>(hit_in[s][c] * 1000u / tot_in[s]) -
                                          static_cast<int32_t>(hit_out[s][c] * 1000u / tot_out[s]);
                    if (score > best_score) { second = best_score; best_score = score; best = static_cast<int32_t>(c); }
                    else if (score > second) { second = score; }
                }
                if (best < 0 || best_score < MinScore || best_score - second < MinMargin || (used[best / 64] & (UINT64_C(1) << (best % 64))) != 0) { return false; }
                m[s] = static_cast<uint8_t>(best);
                used[best / 64] |= UINT64_C(1) << (best % 64);
                if (best_score < worst) { worst = best_score; }
            }
            /* a slot the game never draws reads a buffer no other slot reads */
            for (uint32_t s = 0, c = 0; s < nslots; ++s) {
                if (((unused >> s) & 1) == 0) { continue; }
                while (c < ncand && (used[c / 64] & (UINT64_C(1) << (c % 64))) != 0) { ++c; }
                m[s] = static_cast<uint8_t>(c < ncand ? c : 0);
                if (c < ncand) { used[c / 64] |= UINT64_C(1) << (c % 64); }
            }
            std::memcpy(out, m, nslots);
            if (weakest != nullptr) { *weakest = worst; }
            if (unused_out != nullptr) { *unused_out = unused; }
            return true;
        }

        bool GaveUp() const { return intervals >= GiveUpAfter || samples >= MaxSamples; }

        /* v0.7.23: why no match - per slot the intervals counted (drawn / not
         * drawn) and the two best candidates with their scores, for the log
         * when it gives up (#70, Maestro's 1080p buffers never matched) */
        int Describe(char *out, size_t n) const {
            int len = std::snprintf(out, n, "%u intervals, %u samples;", intervals, samples);
            for (uint32_t s = 0; s < nslots && len > 0 && static_cast<size_t>(len) < n; ++s) {
                int32_t b1 = -1, b2 = -1, s1 = -100000, s2 = -100000;
                if (tot_in[s] != 0 && tot_out[s] != 0) {
                    for (uint32_t c = 0; c < ncand; ++c) {
                        const int32_t score = static_cast<int32_t>(hit_in[s][c] * 1000u / tot_in[s]) -
                                              static_cast<int32_t>(hit_out[s][c] * 1000u / tot_out[s]);
                        if (score > s1) { b2 = b1; s2 = s1; b1 = static_cast<int32_t>(c); s1 = score; }
                        else if (score > s2) { b2 = static_cast<int32_t>(c); s2 = score; }
                    }
                }
                len += std::snprintf(out + len, n - len, " slot %u: drawn in %u, not in %u", s, tot_in[s], tot_out[s]);
                if (b1 >= 0 && len > 0 && static_cast<size_t>(len) < n) { len += std::snprintf(out + len, n - len, ", best c%d %d", b1, s1); }
                if (b2 >= 0 && len > 0 && static_cast<size_t>(len) < n) { len += std::snprintf(out + len, n - len, ", next c%d %d", b2, s2); }
                if (len > 0 && static_cast<size_t>(len) < n) { len += std::snprintf(out + len, n - len, ";"); }
            }
            return len;
        }
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
