/*
 * v0.7.14 overlay probe 2 (ovl2): what streaming the overlays would cost, and
 * whether Tesla can be watched without stalling it. Research only - off
 * unless sdmc:/config/switch-frame-tap/probe-ovl.txt exists, and left out of
 * release builds (SFT_OVL_PROBE).
 *
 * Probe 1 (logs/v0714-ovl1-run1, decoded/) found the pixels: overlayDisp draws
 * the volume bar into a 7680 KB region (2 x 1280x720 RGBA8, block-linear,
 * 16-GOB blocks, premultiplied alpha, all zero when hidden); Tesla draws into
 * 2 x 448x720 RGBA4444 (1344 KB, premultiplied). It also found the costs it
 * could not separate: the stream fell to 33-47 fps while it watched
 * overlayDisp, and attaching to Tesla stalled Ultrahand. So, once a game is
 * on screen and streaming, `cycles=` times (probe-ovl.txt, default 4) with
 * `window=` seconds (default 10) per step:
 *   overlayDisp: detached / attached reading 8 KB every 200 ms (is the bar
 *   showing?) / plus 64 KB every 50 ms / plus 512 KB every 50 ms;
 *   Tesla: attached, debug events answered every 5 ms, its region hashed
 *   every 500 ms.
 * Every second: the stream's frames, read and VIC time, the probe's own read
 * time and bytes, whether the bar showed, Tesla's vi:m transactions (its new
 * sessions are wrapped with the probe on - counted, never captured).
 * No floats are formatted (a stratosphere module has no newlib heap).
 */
#include <stratosphere.hpp>
#include <cstdio>
#include <cstring>
#include "applet_mitm_log.hpp"
#include "applet_mitm_dmdbg.hpp"
#include "applet_mitm_nv.hpp"
#include "applet_mitm_uibinder.hpp"
#include "applet_mitm_ovlprobe.hpp"
#include "applet_mitm_overlay.hpp"

#if SFT_OVL_PROBE

namespace ams::mitm::applet {

    namespace {

        constexpr const char *ProbeFile = "sdmc:/config/switch-frame-tap/probe-ovl.txt";
        constexpr const char *ProbeDir  = "sdmc:/config/switch-frame-tap/probe";
        constexpr u64 OverlayDispProgramId = UINT64_C(0x010000000000100C);

        constexpr size_t Block = 0x10000;              /* 64 KB hash blocks */
        constexpr size_t Chunk = 0x4000;
        alignas(0x1000) constinit u8 g_buf[Chunk];
        constexpr u64 BarBufBytes = UINT64_C(1280) * 768 * 4;   /* one overlayDisp buffer */
        constexpr u64 OdRegionBytes = 2 * BarBufBytes;           /* 7680 KB */

        alignas(os::ThreadStackAlignment) constinit u8 g_probe_stack[16_KB];
        constinit os::ThreadType g_probe_thread;

        constinit u32 g_saves = 0;
        constinit u64 g_od_base_last = 0, g_tesla_base_last = 0;
        constinit u32 g_window_s = 10;

        bool ProbeOn() {
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), ProbeFile, fs::OpenMode_Read))) { return false; }
            fs::CloseFile(f);
            return true;
        }
        /* "key=N" in probe-ovl.txt, clamped to lo..hi */
        u32 Setting(const char *key, u32 def, u32 lo, u32 hi) {
            char t[128] = {};
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), ProbeFile, fs::OpenMode_Read))) { return def; }
            size_t n = 0;
            static_cast<void>(fs::ReadFile(std::addressof(n), f, 0, t, sizeof(t) - 1));
            fs::CloseFile(f);
            const char *p = std::strstr(t, key);
            if (p == nullptr) { return def; }
            u32 v = 0;
            for (p += std::strlen(key); *p >= '0' && *p <= '9'; ++p) { v = v * 10 + static_cast<u32>(*p - '0'); }
            return v < lo ? lo : (v > hi ? hi : v);
        }
        /* a game on screen (it presented in the last second) and the stream
         * sending its frames */
        bool GameStreaming() {
            const u32 f0 = g_live_frames.load(std::memory_order_relaxed);
            os::SleepThread(TimeSpan::FromSeconds(1));
            const u64 t = g_app_present_tick.load(std::memory_order_relaxed);
            const bool on_screen = t != 0 && armTicksToNs(armGetSystemTick() - t) < UINT64_C(1000000000);
            return on_screen && g_live_frames.load(std::memory_order_relaxed) - f0 >= 20;
        }
        u64 NowNs() { return armTicksToNs(armGetSystemTick()); }
        u64 PidOf(u64 program_id) {
            os::ProcessId pid{0};
            if (R_FAILED(::ams::pm::dmnt::GetProcessId(std::addressof(pid), ncm::ProgramId{program_id}))) { return 0; }
            return pid.value;
        }
        u32 Fnv(const u8 *p, size_t n, u32 h) {
            for (size_t i = 0; i < n; i += 4) { h = (h ^ (p[i] | (p[i + 1] << 8) | (p[i + 2] << 16) | (static_cast<u32>(p[i + 3]) << 24))) * 16777619u; }
            return h;
        }

        /* the probe's own reads, timed */
        struct Cost { u64 ns = 0, bytes = 0; };
        bool Read(svc::Handle h, Cost &c, void *dst, u64 va, size_t len) {
            const u64 t0 = NowNs();
            const bool ok = DbgRead(h, dst, va, len);
            c.ns += NowNs() - t0; c.bytes += len;
            return ok;
        }
        /* answer pending debug events (a thread starting or ending stops the
         * whole process until we do) */
        struct Pumper { u32 events = 0; u64 last = 0, max_gap = 0; };
        void Pump(svc::Handle h, Pumper &pm) {
            const u64 now = NowNs();
            if (pm.last != 0 && now - pm.last > pm.max_gap) { pm.max_gap = now - pm.last; }
            pm.last = now;
            s32 idx = 0;
            if (R_SUCCEEDED(svc::WaitSynchronization(std::addressof(idx), std::addressof(h), 1, 0))) {
                u32 n = 0;
                static_cast<void>(DbgResume(h, std::addressof(n)));
                pm.events += n != 0 ? n : 1;
            }
        }

        /* the stream's counters at the start of a second */
        struct Sec { u32 f; u64 r, v; u32 tp; };
        Sec SecNow() {
            return { g_live_frames.load(std::memory_order_relaxed), g_live_read_ns.load(std::memory_order_relaxed),
                     g_live_vic_ns.load(std::memory_order_relaxed), g_tesla_presents.load(std::memory_order_relaxed) };
        }
        void SecLog(const char *tag, u32 sec, const Sec &s0, const Cost &c, int vis, const char *extra) {
            const Sec s1 = SecNow();
            const u32 fr = s1.f - s0.f;
            LogLine("ovl2 %-11s %2u: %2u frames, read %4llu us, VIC %4llu us | probe %5llu us %5llu KB | bar %d | tesla vi %u%s",
                    tag, sec, fr,
                    static_cast<unsigned long long>(fr ? (s1.r - s0.r) / fr / 1000 : 0),
                    static_cast<unsigned long long>(fr ? (s1.v - s0.v) / fr / 1000 : 0),
                    static_cast<unsigned long long>(c.ns / 1000), static_cast<unsigned long long>(c.bytes / 1024),
                    vis, s1.tp - s0.tp, extra);
        }

        /* the device-shared region of `size` bytes (or the largest above `min`) */
        bool FindRegion(svc::Handle h, const char *name, u64 size, u64 min, u64 *out_base, u64 *out_size, bool log_all) {
            u64 addr = 0, best = 0, best_size = 0;
            for (u32 guard = 0; guard < 4096; ++guard) {
                svc::MemoryInfo mi = {};
                svc::PageInfo pi = {};
                if (!DbgQuery(h, std::addressof(mi), std::addressof(pi), addr) || mi.size == 0) { break; }
                const bool dev = (static_cast<u32>(mi.attribute) & svc::MemoryAttribute_DeviceShared) != 0 || mi.device_count != 0;
                if (dev && mi.state != svc::MemoryState_Io && (mi.permission & svc::MemoryPermission_Read) != 0) {
                    if (log_all) {
                        LogLine("ovl2: %s device-shared %010llx %6llu KB dev %u", name, static_cast<unsigned long long>(mi.base_address),
                                static_cast<unsigned long long>(mi.size / 1024), mi.device_count);
                    }
                    if (mi.size == size) { best = mi.base_address; best_size = mi.size; break; }
                    if (mi.size >= min && mi.size > best_size) { best = mi.base_address; best_size = mi.size; }
                }
                const u64 next = mi.base_address + mi.size;
                if (next <= addr) { break; }
                addr = next;
            }
            *out_base = best; *out_size = best_size;
            return best != 0;
        }

        void Save(svc::Handle h, const char *name, u64 base, u64 size, Pumper &pm) {
            if (g_saves >= 6) { return; }
            char path[128];
            std::snprintf(path, sizeof(path), "%s/ovl2-%s-%010llx-%u.bin", ProbeDir, name, static_cast<unsigned long long>(base), g_saves++);
            static_cast<void>(fs::DeleteFile(path));
            if (R_FAILED(fs::CreateFile(path, static_cast<s64>(size)))) { return; }
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), path, fs::OpenMode_Write))) { return; }
            for (u64 off = 0; off < size; off += Chunk) {
                const size_t len = static_cast<size_t>(size - off < Chunk ? size - off : Chunk);
                if (!DbgRead(h, g_buf, base + off, len)) { std::memset(g_buf, 0, len); }
                static_cast<void>(fs::WriteFile(f, static_cast<s64>(off), g_buf, len, fs::WriteOption::None));
                if ((off & 0x3FFFF) == 0) { Pump(h, pm); }
            }
            static_cast<void>(fs::FlushFile(f));
            fs::CloseFile(f);
            LogLine("ovl2: %s saved %s", name, path);
        }

        /* is the volume bar (or anything in the top-left corner) showing? The
         * first 4 KB of a buffer is rows 0-63 of its 16 leftmost pixels; the
         * bar's box covers them, and a hidden buffer is all zero */
        int BarShowing(svc::Handle h, u64 base, Cost &c) {
            int vis = 0;
            for (u32 b = 0; b < 2; ++b) {
                if (!Read(h, c, g_buf, base + b * BarBufBytes, 0x1000)) { return -1; }
                for (u32 i = 3; i < 0x1000; i += 4) { if (g_buf[i] != 0) { vis = 1; break; } }
            }
            return vis;
        }

        enum Mode { Detached, Idle, Light, Heavy };
        constexpr const char *ModeName[] = { "od-detached", "od-idle", "od-light", "od-heavy" };

        void OverlayDispWindow(Mode mode) {
            const char *tag = ModeName[mode];
            if (mode == Detached) {
                for (u32 sec = 0; sec < g_window_s; ++sec) {
                    const Sec s0 = SecNow();
                    os::SleepThread(TimeSpan::FromSeconds(1));
                    SecLog(tag, sec, s0, Cost{}, -1, "");
                }
                return;
            }
            const u64 pid = PidOf(OverlayDispProgramId);
            if (pid == 0) { LogLine("ovl2: overlayDisp is not running"); os::SleepThread(TimeSpan::FromSeconds(g_window_s)); return; }
            svc::Handle h = svc::InvalidHandle;
            if (const Result rc = svc::DebugActiveProcess(std::addressof(h), pid); R_FAILED(rc)) {
                LogLine("ovl2: overlayDisp - DebugActiveProcess rc=0x%x", rc.GetValue());
                os::SleepThread(TimeSpan::FromSeconds(g_window_s));
                return;
            }
            u32 nev = 0;
            static_cast<void>(DbgResume(h, std::addressof(nev)));
            Pumper pm;
            u64 base = 0, size = 0;
            const bool found = FindRegion(h, "overlayDisp", OdRegionBytes, OdRegionBytes, std::addressof(base), std::addressof(size), g_od_base_last == 0);
            if (!found) {
                LogLine("ovl2: overlayDisp - no %llu KB region", static_cast<unsigned long long>(OdRegionBytes / 1024));
            } else if (base != g_od_base_last) {
                LogLine("ovl2: overlayDisp region %010llx (%llu KB)%s", static_cast<unsigned long long>(base), static_cast<unsigned long long>(size / 1024),
                        g_od_base_last != 0 ? " - MOVED" : "");
                g_od_base_last = base;
            }
            const size_t bulk = mode == Heavy ? 512 * 1024 : (mode == Light ? 64 * 1024 : 0);
            u64 bulk_off = 0;
            int last_vis = 0;
            const u64 t_start = NowNs();
            for (u32 sec = 0; sec < g_window_s && found; ++sec) {
                const Sec s0 = SecNow();
                Cost c;
                int vis = 0;
                bool saved = false;
                for (u32 tick = 0; tick < 20; ++tick) {
                    const u64 due = t_start + (static_cast<u64>(sec) * 20 + tick + 1) * 50000000;
                    Pump(h, pm);
                    if ((tick % 4) == 0) {
                        const int v = BarShowing(h, base, c);
                        if (v > vis) { vis = v; }
                        if (v == 1 && last_vis == 0 && !saved) { Save(h, "overlayDisp", base, size, pm); saved = true; }
                        if (v >= 0) { last_vis = v; }
                    }
                    for (size_t done = 0; done < bulk; done += Chunk) {
                        static_cast<void>(Read(h, c, g_buf, base + bulk_off, Chunk));
                        bulk_off = (bulk_off + Chunk) % size;
                    }
                    const u64 now = NowNs();
                    if (now < due) { os::SleepThread(TimeSpan::FromNanoSeconds(static_cast<s64>(due - now))); }
                }
                SecLog(tag, sec, s0, c, vis, saved ? " (saved a snapshot)" : "");
            }
            svc::CloseHandle(h);
            if (pm.events != 0) { LogLine("ovl2: overlayDisp detached - %u debug events", pm.events); }
        }

        void TeslaWindow() {
            const u64 pid = PidOf(TeslaProgramId);
            if (pid == 0) {
                for (u32 sec = 0; sec < g_window_s; ++sec) {
                    const Sec s0 = SecNow();
                    os::SleepThread(TimeSpan::FromSeconds(1));
                    SecLog("tesla-off", sec, s0, Cost{}, -1, "");
                }
                return;
            }
            svc::Handle h = svc::InvalidHandle;
            if (const Result rc = svc::DebugActiveProcess(std::addressof(h), pid); R_FAILED(rc)) {
                LogLine("ovl2: tesla - DebugActiveProcess rc=0x%x", rc.GetValue());
                os::SleepThread(TimeSpan::FromSeconds(g_window_s));
                return;
            }
            u32 nev = 0;
            static_cast<void>(DbgResume(h, std::addressof(nev)));
            Pumper pm;
            pm.last = NowNs();
            u64 base = 0, size = 0;
            const bool found = FindRegion(h, "tesla", 0, 1024 * 1024, std::addressof(base), std::addressof(size), g_tesla_base_last == 0);
            if (found && base != g_tesla_base_last) {
                LogLine("ovl2: tesla region %010llx (%llu KB)%s", static_cast<unsigned long long>(base), static_cast<unsigned long long>(size / 1024),
                        g_tesla_base_last != 0 ? " - MOVED (a new buffer)" : "");
                g_tesla_base_last = base;
            }
            constexpr u32 MaxBlocks = 32;
            u32 hashes[MaxBlocks] = {};
            const u32 nb = found ? static_cast<u32>(((size + Block - 1) / Block) < MaxBlocks ? (size + Block - 1) / Block : MaxBlocks) : 0;
            bool first_hash = true, saved = false;
            const u64 t_start = NowNs();
            for (u32 sec = 0; sec < g_window_s; ++sec) {
                const Sec s0 = SecNow();
                Cost c;
                u32 changed = 0;
                const u32 ev0 = pm.events;
                pm.max_gap = 0;
                for (u32 tick = 0; tick < 200; ++tick) {   /* 5 ms */
                    const u64 due = t_start + (static_cast<u64>(sec) * 200 + tick + 1) * 5000000;
                    Pump(h, pm);
                    if (found && (tick % 100) == 0) {
                        for (u32 b = 0; b < nb; ++b) {
                            u32 v = 2166136261u;
                            const u64 boff = static_cast<u64>(b) * Block;
                            for (u64 o = 0; o < Block && boff + o < size; o += Chunk) {
                                const size_t len = static_cast<size_t>(size - (boff + o) < Chunk ? size - (boff + o) : Chunk);
                                if (Read(h, c, g_buf, base + boff + o, len)) { v = Fnv(g_buf, len, v); }
                            }
                            if (!first_hash && v != hashes[b]) { ++changed; }
                            hashes[b] = v;
                            Pump(h, pm);   /* between blocks, so a thread event never waits a whole pass */
                        }
                        first_hash = false;
                    }
                    const u64 now = NowNs();
                    if (now < due) { os::SleepThread(TimeSpan::FromNanoSeconds(static_cast<s64>(due - now))); }
                }
                char extra[96];
                std::snprintf(extra, sizeof(extra), " | events %u, longest gap %llu us, changed %u blocks%s",
                              pm.events - ev0, static_cast<unsigned long long>(pm.max_gap / 1000), changed,
                              (changed != 0 && !saved && g_saves < 6) ? " (saved a snapshot)" : "");
                if (changed != 0 && !saved && g_saves < 6) { Save(h, "tesla", base, size, pm); saved = true; }
                SecLog("tesla", sec, s0, c, -1, extra);
            }
            svc::CloseHandle(h);
            LogLine("ovl2: tesla detached - %u debug events in the window", pm.events);
        }

        void ProbeThread(void *) {
            os::SleepThread(TimeSpan::FromSeconds(40));
            if (!ProbeOn()) { return; }
            const u32 cycles = Setting("cycles=", 4, 1, 20);
            g_window_s = Setting("window=", 10, 5, 30);
            static_cast<void>(fs::CreateDirectory(ProbeDir));
            LogLine("ovl2: on (probe 2) - once a game streams, %u cycles of %u s steps: overlayDisp detached / idle / light / heavy, then Tesla", cycles, g_window_s);
            /* the measurements mean something only with a game streaming: wait
             * for one (up to 15 minutes), then 5 s for it to settle */
            u32 waited = 0;
            while (!GameStreaming()) {
                if (++waited >= 900) { LogLine("ovl2: no game streamed in 15 minutes - not measuring"); return; }
            }
            LogLine("ovl2: a game is streaming - measuring from now (%u s)", cycles * 5 * g_window_s);
            os::SleepThread(TimeSpan::FromSeconds(5));
            for (u32 cycle = 0; cycle < cycles; ++cycle) {
                LogLine("ovl2: cycle %u", cycle);
                for (u32 m = 0; m < 4; ++m) { OverlayDispWindow(static_cast<Mode>(m)); }
                TeslaWindow();
            }
            LogLine("ovl2: done (%u Tesla vi:m transactions, %u presents)", g_tesla_txns.load(), g_tesla_presents.load());
        }

    }

    void OvlProbeStart() {
        SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_probe_thread), ProbeThread, nullptr, g_probe_stack, sizeof(g_probe_stack), ThreadBasePriority()));
        os::SetThreadNamePointer(std::addressof(g_probe_thread), "applet-mitm.OvlProbe");
        os::StartThread(std::addressof(g_probe_thread));
        NoteThread(std::addressof(g_probe_thread));
    }

}

#else

namespace ams::mitm::applet {

    void OvlProbeStart() { }

}

#endif
