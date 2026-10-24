#include "applet_mitm_uisource.hpp"
#include "applet_mitm_featmem.hpp"
#include <cstdio>
#include <cstring>
#include "applet_mitm_log.hpp"
#include "applet_mitm_dmdbg.hpp"

namespace ams::mitm::applet {

    constinit bool g_ui_stream_armed = false;

    namespace {

        constexpr u64 ViProgramId = UINT64_C(0x010000000000002D);
        constexpr u32 SlotW = 1280, SlotH = 720, SlotRows = 768;
        constexpr u32 SlotBytes = SlotW * SlotRows * 4;   /* 3,840 KB */
        constexpr u32 NumSlots = 7;

        /* PresentSharedFrameBuffer's arguments around the slot word (probe6):
         * fence {count, {id, value} x4} at -0x48, crop -0x24, transform
         * -0x14, swap interval -0x10, pad -0x0C, layer -0x08, slot +0; the
         * CMIF header before them: "SFCI" at -0x58, command 8255 at -0x50 -
         * or, once vi has answered in the same buffer, "SFCO" and result 0
         * (the arguments stay). test1: without the header check, words that
         * only looked like a present (syncpoint 0 or 127) were followed and
         * the capture buffers (slots 0-2) streamed. */
        constexpr u32 MagicSfci = 0x49434653, MagicSfco = 0x4f434653;
        struct Msg {
            u64 va;
            u64 layer;
            u32 fence_id, fence_value;
            s32 slot;
            u64 changed_ns;
        };

        bool ParseAt(const u8 *p, Msg *m) {
            u32 cnt, id, val, swap, pad, magic, cmd;
            u64 layer, slot;
            std::memcpy(std::addressof(magic), p - 0x58, 4);
            std::memcpy(std::addressof(cmd), p - 0x50, 4);
            if (!((magic == MagicSfci && cmd == 8255) || (magic == MagicSfco && cmd == 0))) { return false; }
            std::memcpy(std::addressof(cnt), p - 0x48, 4);
            std::memcpy(std::addressof(id), p - 0x44, 4);
            std::memcpy(std::addressof(val), p - 0x40, 4);
            std::memcpy(std::addressof(swap), p - 0x10, 4);
            std::memcpy(std::addressof(pad), p - 0x0C, 4);
            std::memcpy(std::addressof(layer), p - 0x08, 8);
            std::memcpy(std::addressof(slot), p, 8);
            if (cnt < 1 || cnt > 4 || id == 0 || id >= 192 || swap > 4 || pad != 0) { return false; }
            if (layer == 0 || layer >= 0x1000 || slot >= NumSlots) { return false; }
            m->layer = layer;
            m->fence_id = id;
            m->fence_value = val;
            m->slot = static_cast<s32>(slot);
            return true;
        }

        constexpr u32 MaxMsgs = 64;
        constexpr u32 MaxRegs = 48;
        constexpr size_t Chunk = 16 * 1024;

        struct Region { u64 addr, size; };

        constinit ::ams::svc::Handle g_dbg = ::ams::svc::InvalidHandle;
        constinit u64 g_base = 0;
        /* the scan's buffers (20 KB): taken at the first HOME-menu session,
         * not static - they cost everyone (applet_mitm_featmem.hpp) */
        struct Work {
            Region regs[MaxRegs];
            Msg msgs[MaxMsgs];
            alignas(0x10) u8 chunk[Chunk + 0x60];
        };
        constinit Work *g_work = nullptr;
        constinit u32 g_nregs = 0;
        constinit u32 g_nmsgs = 0;
        constinit s32 g_active = -1;

        constinit u32 g_fed = 0, g_switches = 0, g_rescans = 0, g_shaped = 0;

        constinit bool g_ui_idle = false;   /* v0.7.16: the last start found nothing drawing */
        /* test2: per 5 s window, the layer fed and the slots its frames named */
        constinit u32 g_win_fed = 0, g_win_slots[8] = {};
        constinit u64 g_win_layer = 0;

        alignas(os::ThreadStackAlignment) constinit u8 g_feed_stack[12_KB];
        constinit os::ThreadType g_feed_thread;
        constinit bool g_feed_running = false;
        constinit std::atomic<bool> g_feed_stop{false};

        u64 NowNs() { return armTicksToNs(armGetSystemTick()); }

        /* vi's writable data but the shared buffer, at most 16 MB */
        void FindRegions() {
            g_nregs = 0;
            u64 addr = 0, total = 0;
            for (u32 guard = 0; guard < 4096 && g_nregs < MaxRegs; ++guard) {
                ::ams::svc::MemoryInfo mi = {};
                ::ams::svc::PageInfo pi = {};
                if (!DbgQuery(g_dbg, std::addressof(mi), std::addressof(pi), addr) || mi.size == 0) { break; }
                const u32 type = static_cast<u32>(mi.state) & 0xFF;
                const bool data = type == 0x04 || type == 0x05 || type == 0x09;
                const bool rw = (static_cast<u32>(mi.permission) & 2) != 0;
                if (data && rw && mi.base_address != g_base && mi.size <= 16 * 1024 * 1024 && total + mi.size <= 16 * 1024 * 1024) {
                    g_work->regs[g_nregs++] = { mi.base_address, mi.size };
                    total += mi.size;
                }
                const u64 next = mi.base_address + mi.size;
                if (next <= addr) { break; }
                addr = next;
            }
        }

        /* every present-shaped message in vi's data */
        void Scan() {
            ++g_rescans;
            const u64 keep = g_active >= 0 ? g_work->msgs[g_active].va : 0;   /* the followed one stays followed */
            g_nmsgs = 0;
            g_active = -1;
            g_shaped = 0;
            for (u32 r = 0; r < g_nregs; ++r) {
                /* each chunk read with the 0x58 bytes before it, so a message on a boundary is whole */
                for (u64 off = 0; off < g_work->regs[r].size; off += Chunk) {
                    const u64 lead = off >= 0x58 ? 0x58 : off;
                    const size_t len = static_cast<size_t>(std::min<u64>(Chunk, g_work->regs[r].size - off) + lead);
                    if (!DbgRead(g_dbg, g_work->chunk, g_work->regs[r].addr + off - lead, len)) { continue; }
                    for (size_t i = 0x58; i + 8 <= len; i += 4) {
                        if (i < lead) { continue; }
                        Msg m;
                        if (!ParseAt(g_work->chunk + i, std::addressof(m))) { continue; }
                        ++g_shaped;
                        if (g_nmsgs < MaxMsgs) {
                            m.va = g_work->regs[r].addr + off - lead + i;
                            m.changed_ns = 0;
                            if (m.va == keep) { g_active = static_cast<s32>(g_nmsgs); }
                            g_work->msgs[g_nmsgs++] = m;
                        }
                    }
                }
            }
        }

        bool ReadMsg(u32 k, Msg *out) {
            u8 buf[0x60];
            if (!DbgRead(g_dbg, buf, g_work->msgs[k].va - 0x58, sizeof(buf))) { return false; }
            return ParseAt(buf + 0x58, out);
        }

        void Feed(const Msg &m) {
            const u64 fence[4] = { (static_cast<u64>(m.fence_id) << 32) | m.fence_value, 0, 0, 0 };
            static constexpr s32 NoCrop[4] = { 0, 0, 0, 0 };
            RecordPresent(m.slot, true, 1, fence, 0, NoCrop);
            ++g_fed;
            ++g_win_fed;
            g_win_layer = m.layer;
            if (m.slot >= 0 && m.slot < 8) { ++g_win_slots[m.slot]; }
        }

        /* ~1 ms: the followed message; every 20 ms: all of them (another
         * layer came to the front); nothing moving for 2 s: scan again */
        void FeedThread(void *) {
            const u64 t_start = NowNs();
            u64 last_all = 0, last_any = NowNs(), last_scan = NowNs(), last_win = NowNs();
            while (!g_feed_stop.load(std::memory_order_relaxed)) {
                const u64 now = NowNs();
                if (g_active >= 0) {
                    Msg m;
                    if (ReadMsg(static_cast<u32>(g_active), std::addressof(m)) && m.fence_value != g_work->msgs[g_active].fence_value) {
                        m.va = g_work->msgs[g_active].va;
                        m.changed_ns = now;
                        g_work->msgs[g_active] = m;
                        last_any = now;
                        Feed(m);
                    }
                }
                if (now - last_all >= UINT64_C(20000000)) {
                    last_all = now;
                    for (u32 k = 0; k < g_nmsgs; ++k) {
                        if (static_cast<s32>(k) == g_active) { continue; }
                        Msg m;
                        if (!ReadMsg(k, std::addressof(m)) || m.fence_value == g_work->msgs[k].fence_value) { continue; }
                        m.va = g_work->msgs[k].va;
                        m.changed_ns = now;
                        g_work->msgs[k] = m;
                        last_any = now;
                        /* follow it if the followed one has gone quiet (another layer in front) */
                        if (g_active < 0 || now - g_work->msgs[g_active].changed_ns > UINT64_C(60000000)) {
                            if (g_switches++ < 64) {
                                LogLine("   ui: following layer %#llx (syncpoint %u, slot %d) at %#llx", static_cast<unsigned long long>(m.layer), m.fence_id,
                                        m.slot, static_cast<unsigned long long>(m.va));
                            }
                            g_active = static_cast<s32>(k);
                            Feed(m);
                        }
                    }
                }
                if (now - last_win >= UINT64_C(5000000000)) {
                    last_win = now;
                    LogLine("   ui: 5 s: %u frames fed from layer %#llx, slots 0-6: %u %u %u %u %u %u %u", g_win_fed, static_cast<unsigned long long>(g_win_layer),
                            g_win_slots[0], g_win_slots[1], g_win_slots[2], g_win_slots[3], g_win_slots[4], g_win_slots[5], g_win_slots[6]);
                    g_win_fed = 0;
                    for (u32 k = 0; k < 8; ++k) { g_win_slots[k] = 0; }
                }
                /* nothing found yet: again every 200 ms for the session's first 3 s
                 * (a scan reads ~7 MB of vi - not something to repeat forever) */
                const bool none = g_active < 0 && now - t_start < UINT64_C(3000000000) && now - last_scan > UINT64_C(200000000);
                if (none || (now - last_any > UINT64_C(2000000000) && now - last_scan > UINT64_C(2000000000))) {
                    last_scan = now;
                    Scan();
                }
                os::SleepThread(TimeSpan::FromMicroSeconds(900));
            }
        }

    }

    bool UiSourceFind(u64 *out_pid, u64 *out_base, GameSurface *out_geo) {
        os::ProcessId pid{};
        if (R_FAILED(::ams::pm::dmnt::GetProcessId(std::addressof(pid), ncm::ProgramId{ViProgramId})) || pid.value == 0) { return false; }
        os::NativeHandle h = os::InvalidNativeHandle;
        ncm::ProgramLocation loc = {};
        cfg::OverrideStatus st = {};
        if (R_FAILED(::ams::pm::dmnt::AtmosphereGetProcessInfo(std::addressof(h), std::addressof(loc), std::addressof(st), pid))) { return false; }
        /* the shared buffer: one region mapped by two devices, exactly 7 slots */
        u64 addr = 0, base = 0;
        for (u32 guard = 0; guard < 4096; ++guard) {
            ::MemoryInfo mi = {};
            u32 pi = 0;
            if (R_FAILED(::svcQueryProcessMemory(&mi, &pi, h, addr)) || mi.size == 0) { break; }
            if (mi.device_refcount >= 2 && mi.size == static_cast<u64>(SlotBytes) * NumSlots) { base = mi.addr; break; }
            const u64 next = mi.addr + mi.size;
            if (next <= addr) { break; }
            addr = next;
        }
        os::CloseNativeHandle(h);
        if (base == 0) { return false; }
        GameSurface g = {};
        g.width = SlotW;
        g.height = SlotH;
        g.stride_px = SlotW;
        g.block_h_log2 = 4;
        for (u32 k = 0; k < NumSlots; ++k) { g.slot_offset[k] = k * SlotBytes; g.slot_nvmap[k] = 1; }
        g.num_slots = NumSlots;
        g.buf_size = SlotBytes;
        g.color_format = 0x100532120ull;   /* A8B8G8R8 */
        g.layout = 3;                      /* block-linear */
        g.generation = g_game_surface.generation;   /* a game's new buffers end the session */
        g.armed = true;
        *out_pid = pid.value;
        *out_base = base;
        *out_geo = g;
        return true;
    }

    bool UiSourceIdle() { return g_ui_idle; }

    bool UiSourceStart(::ams::svc::Handle dbg, u64 pid, u64 base) {
        AMS_UNUSED(pid);
        if (g_work == nullptr) {
            static_assert(alignof(Work) <= os::MemoryPageSize);
            void *mem = FeatureMemory(sizeof(Work), "home menu: the present scan");
            if (mem == nullptr) { return false; }
            g_work = static_cast<Work *>(mem);
        }
        g_dbg = dbg;
        g_base = base;
        g_fed = g_switches = g_rescans = 0;
        FindRegions();
        const u64 t0 = NowNs();
        Scan();
        /* the live one: its fence moves within a few frames */
        os::SleepThread(TimeSpan::FromMilliSeconds(60));
        for (u32 k = 0; k < g_nmsgs; ++k) {
            Msg m;
            if (ReadMsg(k, std::addressof(m)) && m.fence_value != g_work->msgs[k].fence_value) {
                m.va = g_work->msgs[k].va;
                m.changed_ns = NowNs();
                g_work->msgs[k] = m;
                if (g_active < 0) { g_active = static_cast<s32>(k); }
            }
        }
        /* v0.7.16: is it really drawing? A game's loading pause ends its
         * session too (Duke Nukem: ~5 s) - the layer found then moved once
         * and never again, and its old picture was all the viewer got in
         * place of the loading screen. The HOME menu, there for real, draws
         * frame after frame: two more moves within 160 ms. */
        g_ui_idle = false;
        if (g_active >= 0) {
            u32 moves = 0;
            u32 fv = g_work->msgs[g_active].fence_value;
            for (u32 i = 0; i < 8 && moves < 2; ++i) {
                os::SleepThread(TimeSpan::FromMilliSeconds(20));
                Msg m;
                if (ReadMsg(static_cast<u32>(g_active), std::addressof(m)) && m.fence_value != fv) { fv = m.fence_value; ++moves; }
            }
            if (moves < 2) { g_ui_idle = true; }
        } else {
            g_ui_idle = true;
        }
        if (g_ui_idle) {
            static u32 said = 0;
            if (said++ < 3) { LogLine("   ui: nothing of the HOME menu is drawing (a game's loading pause?) - not streaming it"); }
            g_active = -1;
            return false;
        }
        LogLine("   ui: vi's shared buffer at %#llx; %u data regions, %u present message(s) (%u shaped), scan %llu ms; %s",
                static_cast<unsigned long long>(base), g_nregs, g_nmsgs, g_shaped, static_cast<unsigned long long>((NowNs() - t0) / 1000000),
                g_active >= 0 ? "one is live" : "none live yet");
        if (g_active >= 0) {
            LogLine("   ui: following layer %#llx (syncpoint %u) at %#llx", static_cast<unsigned long long>(g_work->msgs[g_active].layer), g_work->msgs[g_active].fence_id,
                    static_cast<unsigned long long>(g_work->msgs[g_active].va));
            Feed(g_work->msgs[g_active]);
        }
        g_feed_stop.store(false, std::memory_order_relaxed);
        if (R_FAILED(os::CreateThread(std::addressof(g_feed_thread), FeedThread, nullptr, g_feed_stack, sizeof(g_feed_stack), os::GetThreadPriority(os::GetCurrentThread())))) {
            LogLine("   ui: feeder thread not created");
            return false;
        }
        os::SetThreadNamePointer(std::addressof(g_feed_thread), "applet-mitm.UiFeed");
        os::StartThread(std::addressof(g_feed_thread));
        g_feed_running = true;
        return true;
    }

    void UiSourceStop() {
        if (!g_feed_running) { return; }
        g_feed_stop.store(true, std::memory_order_relaxed);
        os::WaitThread(std::addressof(g_feed_thread));
        os::DestroyThread(std::addressof(g_feed_thread));
        g_feed_running = false;
        g_dbg = ::ams::svc::InvalidHandle;
    }

    void UiSourceLogStats(const char *who) {
        LogLine("   %s: HOME-menu presents fed %u, layer switches %u, scans %u", who, g_fed, g_switches, g_rescans);
    }

}
