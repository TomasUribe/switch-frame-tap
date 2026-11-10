/*
 * v0.7.14 sound probe 3: sound outside games. Research only - off unless
 * sdmc:/config/switch-frame-tap/probe-sound.txt exists, and left out of
 * release builds (SFT_SOUND_PROBE).
 *
 * probe 1 printed floats and stopped the console (see the switch-homebrew
 * skill: no %f in a stratosphere module). probe 2 (logs/v0714-sound2): the
 * final output recorder (audrec:u) gives exact zeros on the HOME menu even
 * with qlaunch's record and master volumes at 1.0 (aud:a 8/4 read 1000/1000;
 * in a game am sets qlaunch's to 0 and the game's to 1.0), and the game's
 * sound with a game - it never carries the system's own sounds.
 *
 * probe 3: the console's final output must be somewhere the audio sysmodule
 * (0100000000000014) can see, as the HOME menu's frames were in vi. Attached
 * as a debugger (as vi is for the picture), three passes of ~20 s:
 *   scroll  100 s after boot - the HOME menu while the user scrolls (clicks)
 *   idle    180 s - the HOME menu untouched (silence)
 *   game    260 s - a game running
 * each one: the memory map (first pass), the pages that keep changing (6
 * hashes 8 ms apart), then for the busiest ranges 1.5 s of samples ~1 ms
 * apart - which 256-byte blocks changed (a ring's write head moves along)
 * and their loudest 16-bit value - and one snapshot of each range to
 * probe/aud-<pass>-r<N>.bin. No floats are formatted anywhere.
 */
#include <stratosphere.hpp>
#include <cstdio>
#include <cstring>
#include "applet_mitm_log.hpp"
#include "applet_mitm_dmdbg.hpp"
#include "applet_mitm_soundprobe.hpp"

#if SFT_SOUND_PROBE

namespace ams::mitm::applet {

    namespace {

        constexpr u64 AudioId = UINT64_C(0x0100000000000014);
        constexpr const char *ProbeFile = "sdmc:/config/switch-frame-tap/probe-sound.txt";
        constexpr const char *ProbeDir  = "sdmc:/config/switch-frame-tap/probe";

        constexpr size_t Page = 0x1000;
        constexpr size_t MaxPages = 16384;            /* 64 MB hashed per pass */
        constinit u32 g_hash[MaxPages];
        constinit u8  g_cnt[MaxPages];

        struct Region { u64 base; u64 size; u32 page0; u32 state; u32 attr; };
        constexpr size_t MaxRegions = 160;
        constinit Region g_regs[MaxRegions];
        constinit u32 g_nregs = 0;
        constinit u32 g_npages = 0;

        constexpr size_t Chunk = 0x4000;
        alignas(0x1000) constinit u8 g_buf[Chunk];

        struct Range { u64 va; u64 size; u32 state; u32 attr; };
        constexpr size_t MaxRanges = 64;
        constinit Range g_ranges[MaxRanges];
        constinit u32 g_nranges = 0;

        constexpr size_t Block = 0x100;
        constexpr size_t MaxSample = 0x40000;         /* 256 KB of a range followed */
        constexpr size_t MaxBlocks = MaxSample / Block;
        constinit u32 g_bh[MaxBlocks];

        constinit u32 g_pumped = 0;

        alignas(os::ThreadStackAlignment) constinit u8 g_probe_stack[16_KB];
        constinit os::ThreadType g_probe_thread;

        bool ProbeOn() {
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), ProbeFile, fs::OpenMode_Read))) { return false; }
            fs::CloseFile(f);
            return true;
        }

        u64 PidOf(u64 program_id) {
            os::ProcessId pid{0};
            if (R_FAILED(::ams::pm::dmnt::GetProcessId(std::addressof(pid), ncm::ProgramId{program_id}))) { return 0; }
            return pid.value;
        }

        u32 Fnv(const u8 *p, size_t n) {
            u32 h = 2166136261u;
            for (size_t i = 0; i < n; ++i) { h = (h ^ p[i]) * 16777619u; }
            return h;
        }

        /* thread starts and exits stop the audio process until continued */
        void Pump(svc::Handle h) {
            s32 idx = 0;
            if (R_SUCCEEDED(svc::WaitSynchronization(std::addressof(idx), std::addressof(h), 1, 0))) {
                u32 n = 0;
                static_cast<void>(DbgResume(h, std::addressof(n)));
                ++g_pumped;
            }
        }

        bool Readable(const svc::MemoryInfo &mi) {
            if ((mi.permission & svc::MemoryPermission_Read) == 0) { return false; }
            switch (mi.state) {
                case svc::MemoryState_CodeData:
                case svc::MemoryState_Normal:
                case svc::MemoryState_Shared:
                case svc::MemoryState_Alias:
                case svc::MemoryState_AliasCodeData:
                case svc::MemoryState_Transfered:
                case svc::MemoryState_SharedTransfered:
                    return true;
                default:
                    return false;
            }
        }

        void Map(svc::Handle h, bool log) {
            g_nregs = 0;
            g_npages = 0;
            u64 addr = 0;
            u32 seen = 0;
            for (u32 guard = 0; guard < 4096; ++guard) {
                svc::MemoryInfo mi = {};
                svc::PageInfo pi = {};
                if (!DbgQuery(h, std::addressof(mi), std::addressof(pi), addr) || mi.size == 0) { break; }
                if (mi.state != svc::MemoryState_Free && mi.state != svc::MemoryState_Inaccessible && log) {
                    LogLine("soundprobe: map %010llx %8llu KB state 0x%x perm %u attr 0x%x ipc %u dev %u",
                            static_cast<unsigned long long>(mi.base_address), static_cast<unsigned long long>(mi.size / 1024),
                            static_cast<u32>(mi.state), static_cast<u32>(mi.permission), static_cast<u32>(mi.attribute), mi.ipc_count, mi.device_count);
                }
                const u64 pages = mi.size / Page;
                if (Readable(mi) && mi.size <= (UINT64_C(32) << 20) && g_npages + pages <= MaxPages && g_nregs < MaxRegions) {
                    g_regs[g_nregs++] = { mi.base_address, mi.size, g_npages, static_cast<u32>(mi.state), static_cast<u32>(mi.attribute) };
                    g_npages += static_cast<u32>(pages);
                }
                const u64 next = mi.base_address + mi.size;
                if (next <= addr) { break; }
                addr = next;
                if ((++seen & 31) == 0) { Pump(h); }
            }
        }

        void HashAll(svc::Handle h, bool count) {
            for (u32 r = 0; r < g_nregs; ++r) {
                const Region &g = g_regs[r];
                for (u64 off = 0; off < g.size; off += Chunk) {
                    const size_t n = static_cast<size_t>(g.size - off < Chunk ? g.size - off : Chunk);
                    if (!DbgRead(h, g_buf, g.base + off, n)) { continue; }
                    for (size_t p = 0; p < n; p += Page) {
                        const u32 i = g.page0 + static_cast<u32>((off + p) / Page);
                        const u32 v = Fnv(g_buf + p, Page);
                        if (count && v != g_hash[i] && g_cnt[i] < 255) { ++g_cnt[i]; }
                        g_hash[i] = v;
                    }
                }
                Pump(h);
            }
        }

        void FindRanges(const char *tag) {
            g_nranges = 0;
            u32 logged = 0;
            for (u32 r = 0; r < g_nregs; ++r) {
                const Region &g = g_regs[r];
                const u32 np = static_cast<u32>(g.size / Page);
                for (u32 p = 0; p < np; ) {
                    if (g_cnt[g.page0 + p] < 3) { ++p; continue; }
                    u32 q = p;
                    while (q < np && g_cnt[g.page0 + q] >= 3) { ++q; }
                    const Range rg = { g.base + static_cast<u64>(p) * Page, static_cast<u64>(q - p) * Page, g.state, g.attr };
                    if (logged++ < 60) {
                        LogLine("soundprobe: %s changing %010llx %6llu KB (state 0x%x attr 0x%x)", tag,
                                static_cast<unsigned long long>(rg.va), static_cast<unsigned long long>(rg.size / 1024), rg.state, rg.attr);
                    }
                    if (g_nranges < MaxRanges) { g_ranges[g_nranges++] = rg; }
                    p = q;
                }
            }
            LogLine("soundprobe: %s %u changing range(s)", tag, logged);
        }

        /* the audio sysmodule's own device-shared memory first (where an
         * output ring would be), then the clients' transfer memory; larger first */
        u32 Rank(const Range &r) {
            const bool dev = (r.attr & svc::MemoryAttribute_DeviceShared) != 0;
            const bool own = r.state != svc::MemoryState_Transfered && r.state != svc::MemoryState_SharedTransfered;
            return (dev ? 2u : 0u) + (own ? 1u : 0u);
        }

        void SortRanges() {
            for (u32 i = 1; i < g_nranges; ++i) {
                for (u32 j = i; j > 0; --j) {
                    const Range &a = g_ranges[j - 1], &b = g_ranges[j];
                    const bool swap = Rank(b) > Rank(a) || (Rank(b) == Rank(a) && b.size > a.size);
                    if (!swap) { break; }
                    const Range t = g_ranges[j - 1]; g_ranges[j - 1] = g_ranges[j]; g_ranges[j] = t;
                }
            }
        }

        /* reads the range; returns the changed blocks' span and loudest value */
        void SampleOnce(svc::Handle h, const Range &r, bool first, u32 *out_changed, u32 *out_lo, u32 *out_hi, u32 *out_peak) {
            const size_t len = static_cast<size_t>(r.size < MaxSample ? r.size : MaxSample);
            u32 changed = 0, lo = 0xFFFFFFFF, hi = 0, peak = 0;
            for (size_t off = 0; off < len; off += Chunk) {
                const size_t n = len - off < Chunk ? len - off : Chunk;
                if (!DbgRead(h, g_buf, r.va + off, n)) { continue; }
                for (size_t b = 0; b < n; b += Block) {
                    const u32 bi = static_cast<u32>((off + b) / Block);
                    const u32 v = Fnv(g_buf + b, Block);
                    if (!first && v != g_bh[bi]) {
                        ++changed;
                        if (bi < lo) { lo = bi; }
                        if (bi > hi) { hi = bi; }
                        const s16 *s = reinterpret_cast<const s16 *>(g_buf + b);
                        for (size_t k = 0; k < Block / 2; ++k) {
                            const u32 a = static_cast<u32>(s[k] < 0 ? -static_cast<s32>(s[k]) : s[k]);
                            if (a > peak) { peak = a; }
                        }
                    }
                    g_bh[bi] = v;
                }
            }
            *out_changed = changed; *out_lo = lo; *out_hi = hi; *out_peak = peak;
        }

        void Follow(svc::Handle h, const char *tag, u32 k, const Range &r) {
            u32 c = 0, lo = 0, hi = 0, pk = 0;
            SampleOnce(h, r, true, &c, &lo, &hi, &pk);
            const u64 start = armTicksToNs(armGetSystemTick());
            for (u32 w = 0; w < 15; ++w) {
                const u64 wend = start + static_cast<u64>(w + 1) * 100000000;
                u32 samples = 0, changed_samples = 0, blocks = 0, wlo = 0xFFFFFFFF, whi = 0, wpeak = 0, head = 0xFFFFFFFF;
                while (armTicksToNs(armGetSystemTick()) < wend) {
                    SampleOnce(h, r, false, &c, &lo, &hi, &pk);
                    ++samples;
                    if (c != 0) {
                        ++changed_samples;
                        blocks += c;
                        if (lo < wlo) { wlo = lo; }
                        if (hi > whi) { whi = hi; }
                        if (pk > wpeak) { wpeak = pk; }
                        head = lo;
                    }
                    Pump(h);
                    os::SleepThread(TimeSpan::FromMicroSeconds(800));
                }
                LogLine("soundprobe: %s r%u w%02u: %u samples, %u changed, blocks %u-%u (%u in all), last first-changed %u, peak %u",
                        tag, k, w, samples, changed_samples, wlo == 0xFFFFFFFF ? 0 : wlo, whi, blocks, head == 0xFFFFFFFF ? 0 : head, wpeak);
            }
        }

        void Dump(svc::Handle h, const char *tag, u32 k, const Range &r) {
            char path[96];
            std::snprintf(path, sizeof(path), "%s/aud-%s-r%u.bin", ProbeDir, tag, k);
            const size_t len = static_cast<size_t>(r.size < MaxSample ? r.size : MaxSample);
            static_cast<void>(fs::DeleteFile(path));
            if (R_FAILED(fs::CreateFile(path, static_cast<s64>(len)))) { return; }
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), path, fs::OpenMode_Write))) { return; }
            for (size_t off = 0; off < len; off += Chunk) {
                const size_t n = len - off < Chunk ? len - off : Chunk;
                if (!DbgRead(h, g_buf, r.va + off, n)) { std::memset(g_buf, 0, n); }
                static_cast<void>(fs::WriteFile(f, static_cast<s64>(off), g_buf, n, fs::WriteOption::None));
            }
            static_cast<void>(fs::FlushFile(f));
            fs::CloseFile(f);
        }

        void Pass(const char *tag, bool log_map) {
            const u64 pid = PidOf(AudioId);
            if (pid == 0) { LogLine("soundprobe: %s - the audio sysmodule's pid not found", tag); return; }
            LogLine("soundprobe: %s - attaching to audio (pid %llu)", tag, static_cast<unsigned long long>(pid));
            svc::Handle h = svc::InvalidHandle;
            const Result arc = svc::DebugActiveProcess(std::addressof(h), pid);
            if (R_FAILED(arc)) { LogLine("soundprobe: %s - DebugActiveProcess rc=0x%x", tag, arc.GetValue()); return; }
            u32 nev = 0;
            const u64 t0 = armTicksToNs(armGetSystemTick());
            const Result crc = DbgResume(h, std::addressof(nev));
            LogLine("soundprobe: %s - attached, %u attach events, continue rc=0x%x after %llu us", tag, nev, crc.GetValue(),
                    static_cast<unsigned long long>((armTicksToNs(armGetSystemTick()) - t0) / 1000));
            g_pumped = 0;

            Map(h, log_map);
            LogLine("soundprobe: %s - %u readable regions, %u pages (%u KB) hashed", tag, g_nregs, g_npages, g_npages * 4);
            std::memset(g_cnt, 0, sizeof(g_cnt));
            const u64 s0 = armTicksToNs(armGetSystemTick());
            HashAll(h, false);
            for (int i = 0; i < 5; ++i) {
                os::SleepThread(TimeSpan::FromMilliSeconds(8));
                HashAll(h, true);
            }
            LogLine("soundprobe: %s - 6 hashes in %llu ms", tag, static_cast<unsigned long long>((armTicksToNs(armGetSystemTick()) - s0) / 1000000));
            FindRanges(tag);
            SortRanges();
            const u32 follow = g_nranges < 12 ? g_nranges : 12;
            for (u32 k = 0; k < follow; ++k) {
                const Range &r = g_ranges[k];
                LogLine("soundprobe: %s r%u = %010llx %llu KB (state 0x%x attr 0x%x)", tag, k,
                        static_cast<unsigned long long>(r.va), static_cast<unsigned long long>(r.size / 1024), r.state, r.attr);
                Follow(h, tag, k, r);
            }
            for (u32 k = 0; k < follow; ++k) { Dump(h, tag, k, g_ranges[k]); Pump(h); }
            svc::CloseHandle(h);
            LogLine("soundprobe: %s - detached (%u debug-event wakes while attached), %u snapshots saved", tag, g_pumped, follow);
        }

        void ProbeThread(void *) {
            os::SleepThread(TimeSpan::FromSeconds(40));
            if (!ProbeOn()) { return; }
            static_cast<void>(fs::CreateDirectory(ProbeDir));
            LogLine("soundprobe: on (probe 3) - the audio sysmodule's memory: scroll at ~100 s, idle at ~180 s, game at ~260 s after boot");
            os::SleepThread(TimeSpan::FromSeconds(30));
            Pass("scroll", true);
            os::SleepThread(TimeSpan::FromSeconds(60));
            Pass("idle", false);
            os::SleepThread(TimeSpan::FromSeconds(60));
            Pass("game", false);
            LogLine("soundprobe: done");
        }

    }

    void SoundProbeStart() {
        SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_probe_thread), ProbeThread, nullptr, g_probe_stack, sizeof(g_probe_stack), ThreadBasePriority()));
        os::SetThreadNamePointer(std::addressof(g_probe_thread), "applet-mitm.SndProbe");
        os::StartThread(std::addressof(g_probe_thread));
        NoteThread(std::addressof(g_probe_thread));
    }

}

#else

namespace ams::mitm::applet {

    void SoundProbeStart() { }

}

#endif
