#include "applet_mitm_compositeprobe.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_nv.hpp"
#include "applet_mitm_uisource.hpp"
#include "applet_mitm_dmdbg.hpp"
#include "applet_mitm_sm.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ams::mitm::applet {

    constinit bool g_composite_probe_armed = false;

    namespace {

        constexpr u32 SlotCount = 7;
        constexpr u32 SlotW = 1280, SlotRows = 768;
        constexpr u32 SlotBytes = SlotW * SlotRows * 4;   /* 3,840 KB */
        constexpr size_t Chunk = 64 * 1024;

        /* The module must never take multi-MB static memory (a 4 MB .bss
         * array fataled another sysmodule). Read in 64 KB pieces and keep the
         * 8293 output buffer small: we only need its result code for now. */
        alignas(0x1000) constinit u8 g_probe_chunk[Chunk] = {};
        alignas(0x1000) constinit u8 g_probe_out[0x1000] = {};
        alignas(os::ThreadStackAlignment) constinit u8 g_probe_stack[12_KB];
        constinit os::ThreadType g_probe_thread;

        struct LayerStat {
            s32 session;
            u64 tid;
            u32 sets;
            u32 queues;
            s32 last_slot;
            u64 last_tick;
        };
        constexpr u32 MaxLayers = 24;
        constinit LayerStat g_layers[MaxLayers] = {};
        constinit u32 g_layer_n = 0;
        constinit os::SdkMutex g_layer_lock;

        u64 NowNs() { return armTicksToNs(armGetSystemTick()); }

        u64 HashChunk(u64 h, const u8 *p, size_t n, u64 off) {
            for (size_t i = 0; i + 8 <= n; i += 256) {
                u64 v = 0;
                std::memcpy(std::addressof(v), p + i, 8);
                h = (h ^ v) * 0x100000001b3ull;
            }
            return h ^ off;
        }

        bool ChunkNonBlack(const u8 *p, size_t n) {
            for (size_t i = 0; i + 4 <= n; i += 0x1000) {
                if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 0 || p[i + 3] != 0) { return true; }
            }
            return false;
        }

        /* One slot in 64 KB pieces. *hash and *nonblack are the pass result;
         * when write_path != nullptr the same read is also written there. */
        bool ReadSlot(::ams::svc::Handle dbg, u64 base, u32 slot, u64 *hash, bool *nonblack, const char *write_path) {
            fs::FileHandle f = {};
            const bool wanted = write_path != nullptr;
            bool writing = wanted;
            bool opened = false;
            if (writing) {
                static_cast<void>(fs::DeleteFile(write_path));
                if (R_FAILED(fs::CreateFile(write_path, SlotBytes)) ||
                    R_FAILED(fs::OpenFile(std::addressof(f), write_path, fs::OpenMode_Write))) {
                    writing = false;
                    write_path = nullptr;
                } else {
                    opened = true;
                }
            }
            u64 h = 0xcbf29ce484222325ull;
            bool nb = false;
            bool ok = true;
            for (size_t off = 0; off < SlotBytes; off += Chunk) {
                const size_t n = std::min<size_t>(Chunk, SlotBytes - off);
                if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(g_probe_chunk), dbg,
                                                               base + static_cast<u64>(slot) * SlotBytes + off, n))) {
                    ok = false;
                    break;
                }
                h = HashChunk(h, g_probe_chunk, n, off);
                nb |= ChunkNonBlack(g_probe_chunk, n);
                if (writing && R_FAILED(fs::WriteFile(f, static_cast<s64>(off), g_probe_chunk, n, fs::WriteOption::None))) {
                    writing = false;
                }
            }
            if (opened) { fs::CloseFile(f); }
            if (wanted && !opened) { LogLine("compositeprobe: could not open the raw slot file - hashes only"); }
            if (hash != nullptr) { *hash = h; }
            if (nonblack != nullptr) { *nonblack = nb; }
            return ok;
        }

        void SnapshotSharedBuffer() {
            u64 vpid = 0, base = 0;
            GameSurface geo = {};
            if (!UiSourceFind(std::addressof(vpid), std::addressof(base), std::addressof(geo))) {
                LogLine("compositeprobe: vi's shared buffer not found");
                return;
            }
            LogLine("compositeprobe: vi pid %llu, shared buffer %#llx, %u slots",
                    static_cast<unsigned long long>(vpid), static_cast<unsigned long long>(base),
                    static_cast<unsigned>(geo.num_slots));

            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            if (R_FAILED(::ams::svc::DebugActiveProcess(std::addressof(dbg), vpid))) {
                LogLine("compositeprobe: DebugActiveProcess(vi) failed");
                return;
            }
            u32 nev = 0;
            static_cast<void>(DbgResume(dbg, std::addressof(nev)));
            LogLine("compositeprobe: attached to vi (%u events), reading %u slots", nev, SlotCount);

            static_cast<void>(fs::CreateDirectory("sdmc:/config/switch-frame-tap"));
            const Result mkdir_rc = fs::CreateDirectory("sdmc:/config/switch-frame-tap/probe");
            fs::DirectoryEntryType probe_type = fs::DirectoryEntryType_File;
            const bool dir_ok = R_SUCCEEDED(mkdir_rc) ||
                                (R_SUCCEEDED(fs::GetEntryType(std::addressof(probe_type), "sdmc:/config/switch-frame-tap/probe")) &&
                                 probe_type == fs::DirectoryEntryType_Directory);

            u64 hash1[SlotCount] = {};
            u64 hash2[SlotCount] = {};
            bool ok[SlotCount] = {};
            bool nonblack[SlotCount] = {};

            for (u32 s = 0; s < SlotCount; ++s) {
                ok[s] = ReadSlot(dbg, base, s, std::addressof(hash1[s]), std::addressof(nonblack[s]), nullptr);
            }
            os::SleepThread(TimeSpan::FromMilliSeconds(80));
            for (u32 s = 0; s < SlotCount; ++s) {
                if (!ReadSlot(dbg, base, s, std::addressof(hash2[s]), nullptr, nullptr)) { ok[s] = false; }
            }

            for (u32 s = 0; s < SlotCount; ++s) {
                if (!ok[s]) { LogLine("compositeprobe: slot %u read FAILED", s); continue; }
                LogLine("compositeprobe: slot %u changed=%s nonblack=%s hash=%016llx/%016llx",
                        s, hash1[s] != hash2[s] ? "YES" : "no", nonblack[s] ? "yes" : "no",
                        static_cast<unsigned long long>(hash1[s]), static_cast<unsigned long long>(hash2[s]));
            }

            /* write the slots that moved or hold content; a raw dump is the
             * only way to tell a full composite from a UI layer */
            for (u32 s = 0; s < SlotCount; ++s) {
                if (!dir_ok || !ok[s] || (!nonblack[s] && hash1[s] == hash2[s])) { continue; }
                char path[96];
                std::snprintf(path, sizeof(path), "sdmc:/config/switch-frame-tap/probe/composite-slot%u.raw", s);
                const bool w = ReadSlot(dbg, base, s, nullptr, nullptr, path);
                LogLine("compositeprobe: slot %u -> %s (%s)", s, path, w ? "written" : "FAILED");
            }

            DbgDetach(dbg);
            LogLine("compositeprobe: vi detached");
        }

        void ProbeViManager() {
            ::Service root = {}, disp = {}, sys = {}, mgr = {};
            bool have_disp = false, have_sys = false, have_mgr = false;
            {
                SmUse sm;
                if (R_FAILED(smGetService(std::addressof(root), "vi:m"))) {
                    LogLine("compositeprobe: smGetService(vi:m) failed");
                    return;
                }
                u32 mode = 1;
                const Result r = serviceDispatchIn(std::addressof(root), 2, mode,
                    .out_num_objects = 1, .out_objects = std::addressof(disp));
                have_disp = R_SUCCEEDED(r);
                LogLine("compositeprobe: vi:m GetDisplayService rc=0x%x", r.GetValue());
            }
            if (!have_disp) { serviceClose(std::addressof(root)); return; }

            {
                const Result r1 = serviceDispatch(std::addressof(disp), 101,
                    .out_num_objects = 1, .out_objects = std::addressof(sys));
                const Result r2 = serviceDispatch(std::addressof(disp), 102,
                    .out_num_objects = 1, .out_objects = std::addressof(mgr));
                have_sys = R_SUCCEEDED(r1);
                have_mgr = R_SUCCEEDED(r2);
                LogLine("compositeprobe: GetSystemDisplayService rc=0x%x, GetManagerDisplayService rc=0x%x",
                        r1.GetValue(), r2.GetValue());
            }

            /* 8225: 0x10 in, 0x10 out, output buffer type 22, PID. Ask with
             * zeroes first; a wrong shape must answer, not wedge. */
            if (have_sys) {
                const struct { u64 a, b; } in = { 0, 0 };
                struct { u64 a, b; } out = {};
                const Result r = serviceDispatchInOut(std::addressof(sys), 8225, in, out,
                    .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcAutoSelect },
                    .buffers      = { { g_probe_out, sizeof(g_probe_out) } },
                    .in_send_pid  = true);
                LogLine("compositeprobe: 8225 GetSharedBufferMemoryHandleId rc=0x%x -> %llx %llx",
                        r.GetValue(), static_cast<unsigned long long>(out.a), static_cast<unsigned long long>(out.b));
            }

            /* 8297: 0x10 in, 0x20 out. Zeroes are a safe first shape. */
            if (have_mgr) {
                const struct { u64 a, b; } in = { 0, 0 };
                struct { u64 a, b, c, d; } out = {};
                const Result r = serviceDispatchInOut(std::addressof(mgr), 8297, in, out);
                LogLine("compositeprobe: 8297 GetSharedFrameBufferContentParameter rc=0x%x -> %llx %llx %llx %llx",
                        r.GetValue(), static_cast<unsigned long long>(out.a), static_cast<unsigned long long>(out.b),
                        static_cast<unsigned long long>(out.c), static_cast<unsigned long long>(out.d));

                /* 8293: 0x28 in, 0x8 out, output buffer. The input layout is
                 * not documented; zeroes tell us whether the command is
                 * reachable before we try real handles. */
                std::memset(g_probe_out, 0xAB, sizeof(g_probe_out));
                const struct { u64 a, b, c, d, e; } in93 = { 0, 0, 0, 0, 0 };
                u64 out93 = 0;
                const Result r93 = serviceDispatchInOut(std::addressof(mgr), 8293, in93, out93,
                    .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
                    .buffers      = { { g_probe_out, sizeof(g_probe_out) } });
                LogLine("compositeprobe: 8293 GetDetachedSharedFrameBufferImage rc=0x%x -> %llu (first bytes %02x %02x %02x %02x)",
                        r93.GetValue(), static_cast<unsigned long long>(out93),
                        g_probe_out[0], g_probe_out[1], g_probe_out[2], g_probe_out[3]);
            }

            if (have_mgr) { serviceClose(std::addressof(mgr)); }
            if (have_sys) { serviceClose(std::addressof(sys)); }
            serviceClose(std::addressof(disp));
            serviceClose(std::addressof(root));
        }

        void LayerSummary(const char *why) {
            LayerStat copy[MaxLayers] = {};
            u32 n = 0;
            {
                std::scoped_lock lk(g_layer_lock);
                n = g_layer_n;
                for (u32 i = 0; i < n; ++i) { copy[i] = g_layers[i]; }
            }
            /* log after the lock: the binder thread must never wait on the SD */
            LogLine("compositeprobe: layer census (%s): %u session(s)", why, n);
            for (u32 i = 0; i < n; ++i) {
                const LayerStat &l = copy[i];
                LogLine("compositeprobe:   session %d tid %016llx sets %u queues %u last slot %d age %llu ms",
                        l.session, static_cast<unsigned long long>(l.tid), l.sets, l.queues, l.last_slot,
                        static_cast<unsigned long long>(l.last_tick == 0 ? 0 : (armTicksToNs(armGetSystemTick() - l.last_tick) / 1000000)));
            }
        }

        void ProbeThread(void *) {
            const u64 start = NowNs();
            LogLine("compositeprobe: armed; waiting %u s before the first sample", g_probe_delay_s);
            while (NowNs() - start < static_cast<u64>(g_probe_delay_s) * UINT64_C(1000000000)) {
                os::SleepThread(TimeSpan::FromMilliSeconds(500));
            }
            LogLine("compositeprobe: starting (game should be in gameplay now)");

            ::ams::os::ProcessId pid{};
            for (u32 i = 0; i < 120; ++i) {
                if (g_pmdmnt_rc == 0 && R_SUCCEEDED(::ams::pm::dmnt::GetApplicationProcessId(std::addressof(pid))) && pid.value != 0) { break; }
                os::SleepThread(TimeSpan::FromMilliSeconds(1000));
            }
            const u64 tid = g_app_tid.load(std::memory_order_relaxed);
            LogLine("compositeprobe: application pid %llu, tid %016llx, presents %u",
                    static_cast<unsigned long long>(pid.value), static_cast<unsigned long long>(tid),
                    g_queue_count.load(std::memory_order_relaxed));

            LayerSummary("before snapshot");
            SnapshotSharedBuffer();
            LayerSummary("after snapshot");
            ProbeViManager();
            LayerSummary("after vi:m queries");

            LogLine("compositeprobe: done; raw slots are in sdmc:/config/switch-frame-tap/probe/");
        }

    }

    void CompositeProbeNoteTxn(u64 program_id, s32 session_id, u32 code, s32 slot) {
        if (!g_composite_probe_armed) { return; }
        std::scoped_lock lk(g_layer_lock);
        u32 i = 0;
        while (i < g_layer_n && !(g_layers[i].session == session_id && g_layers[i].tid == program_id)) { ++i; }
        if (i == g_layer_n) {
            if (g_layer_n >= MaxLayers) { return; }
            g_layers[g_layer_n].session = session_id;
            g_layers[g_layer_n].tid = program_id;
            g_layers[g_layer_n].sets = 0;
            g_layers[g_layer_n].queues = 0;
            g_layers[g_layer_n].last_slot = -1;
            g_layers[g_layer_n].last_tick = 0;
            ++g_layer_n;
        }
        LayerStat &l = g_layers[i];
        if (code == 14) { ++l.sets; }
        if (code == 7) { ++l.queues; l.last_slot = slot; l.last_tick = armGetSystemTick(); }
    }

    void StartCompositeProbe() {
        if (!g_composite_probe_armed) { return; }
        const s32 prio = os::GetThreadPriority(os::GetCurrentThread());
        if (R_FAILED(os::CreateThread(std::addressof(g_probe_thread), ProbeThread, nullptr,
                                      g_probe_stack, sizeof(g_probe_stack), prio))) {
            LogLine("compositeprobe: thread not created");
            return;
        }
        os::SetThreadNamePointer(std::addressof(g_probe_thread), "applet-mitm.CompositeProbe");
        os::StartThread(std::addressof(g_probe_thread));
        NoteThread(std::addressof(g_probe_thread));
    }

}
