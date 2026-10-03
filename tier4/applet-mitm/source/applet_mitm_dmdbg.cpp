/*
 * applet-mitm - Option A backend. See applet_mitm_dmdbg.hpp.
 *
 * The dmnt:cht command ids and buffer shapes are copied from Atmosphere's
 * dmntcht.os.horizon.c (65000 HasCheatProcess, 65003 ForceOpenCheatProcess,
 * 65102 ReadCheatProcessMemory, 65104 QueryCheatProcessMemory).
 */
#include "applet_mitm_dmdbg.hpp"
#include "applet_mitm_log.hpp"
#include <cstring>

namespace ams::mitm::applet {

    namespace {

        constinit ::Service g_cht = {};
        constinit bool g_cht_init = false;
        constinit bool g_cht_ok = false;

        /* Bounce reads through plain cached memory in 2 MB pieces - the same
         * chunk size the direct path uses (M96). The frame buffers we read
         * into (g_ind_buf and friends) are nvmap-pinned, and the kernel
         * refuses to map such pages as an IPC buffer into dmnt - the read
         * fails while stack-destination probes of a few dozen bytes succeed.
         * Same lesson as M78 (never hand engine/aliased memory to IPC);
         * one extra memcpy per piece. Smaller pieces were tried first
         * (256 KB): they worked but cost ~35 IPCs per 1080p frame and
         * pushed console latency past 100 ms. */
        alignas(0x1000) constinit u8 g_bounce[0x200000];
        constexpr size_t BouncePiece = sizeof(g_bounce);

        /* dmnt:cht allows 2 sessions; one shared session plus this lock, so a
         * screenshot racing the live loop cannot interleave requests. */
        constinit os::SdkMutex g_cht_lock;

        bool ChtService() {
            if (!g_cht_init) {
                g_cht_init = true;
                const ::Result rc = smGetService(std::addressof(g_cht), "dmnt:cht");
                g_cht_ok = R_SUCCEEDED(rc);
                LogLine("dmdbg: smGetService(dmnt:cht) rc=0x%x %s", rc,
                        g_cht_ok ? "OPEN" : "missing - direct debug only");
            }
            return g_cht_ok;
        }

        bool ChtHasProcess(bool *out) {
            u8 tmp = 0;
            const ::Result rc = serviceDispatchOut(std::addressof(g_cht), 65000, tmp);
            if (R_SUCCEEDED(rc) && out != nullptr) { *out = (tmp & 1) != 0; }
            return R_SUCCEEDED(rc);
        }

        ::Result ChtForceOpen() {
            return serviceDispatch(std::addressof(g_cht), 65003);
        }

        ::Result ChtRead(u64 va, void *dst, size_t len) {
            const struct { u64 address; u64 size; } in = { va, len };
            return serviceDispatchIn(std::addressof(g_cht), 65102, in,
                .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias },
                .buffers = { { dst, len } },
            );
        }

        ::Result ChtQuery(u64 addr, svc::MemoryInfo *mi) {
            return serviceDispatchInOut(std::addressof(g_cht), 65104, addr, *mi);
        }

    }

    /* dmnt first: it stays the game's single debugger (forced open if
     * needed), so cheats keep working. Direct DebugActiveProcess is only a
     * fallback if dmnt:cht is missing. */
    bool DmAttach(u64 pid, svc::Handle *out_dbg) {
        if (ChtService()) {
            std::scoped_lock lk(g_cht_lock);
            bool has = false;
            if (ChtHasProcess(std::addressof(has)) && !has) {
                /* Attaches to any application, cheats or not; a no-op that
                 * succeeds when dmnt already tracks this game. */
                const ::Result fr = ChtForceOpen();
                LogLine("dmdbg: dmnt ForceOpenCheatProcess rc=0x%x", fr);
                ChtHasProcess(std::addressof(has));
            }
            if (has) {
                *out_dbg = kDmntDbg;
                LogLine("dmdbg: attached to pid %llu through dmnt:cht (cheats keep working)",
                        static_cast<unsigned long long>(pid));
                return true;
            }
            LogLine("dmdbg: dmnt has no cheat process for pid %llu - direct fallback",
                    static_cast<unsigned long long>(pid));
        }
        const Result ra = svc::DebugActiveProcess(out_dbg, pid);
        LogLine("dmdbg: direct DebugActiveProcess(pid=%llu) rc=0x%x",
                static_cast<unsigned long long>(pid), ra.GetValue());
        return R_SUCCEEDED(ra);
    }

    bool DbgRead(svc::Handle dbg, void *dst, u64 va, size_t len) {
        if (dst == nullptr || len == 0) { return false; }
        if (DbgIsDmnt(dbg)) {
            std::scoped_lock lk(g_cht_lock);
            u8 *out = static_cast<u8 *>(dst);
            for (size_t off = 0; off < len; off += BouncePiece) {
                const size_t n = (len - off < BouncePiece) ? (len - off) : BouncePiece;
                if (R_FAILED(ChtRead(va + off, g_bounce, n))) { return false; }
                std::memcpy(out + off, g_bounce, n);
            }
            return true;
        }
        return R_SUCCEEDED(svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(dst), dbg, va, len));
    }

    bool DbgQuery(svc::Handle dbg, svc::MemoryInfo *mi, svc::PageInfo *pi, u64 addr) {
        if (mi == nullptr) { return false; }
        if (DbgIsDmnt(dbg)) {
            std::scoped_lock lk(g_cht_lock);
            if (R_FAILED(ChtQuery(addr, mi))) { return false; }
            if (pi != nullptr) { *pi = {}; }
            return true;
        }
        return R_SUCCEEDED(svc::QueryDebugProcessMemory(mi, pi, dbg, addr));
    }

    Result DbgResume(svc::Handle dbg, u32 *out_nev) {
        if (DbgIsDmnt(dbg)) {
            /* ponytail: dmnt pumps its own handle; nothing to drain here */
            if (out_nev != nullptr) { *out_nev = 0; }
            return ResultSuccess();
        }
        svc::DebugEventInfo ev;
        u32 nev = 0;
        while (nev < 256 && R_SUCCEEDED(svc::GetDebugEvent(std::addressof(ev), dbg))) { ++nev; }
        const Result rc = svc::ContinueDebugEvent(dbg,
                              svc::ContinueFlag_ExceptionHandled | svc::ContinueFlag_ContinueAll,
                              nullptr, 0);
        if (out_nev != nullptr) { *out_nev = nev; }
        return rc;
    }

    void DbgDetach(svc::Handle dbg) {
        if (DbgIsDmnt(dbg)) { return; }
        if (dbg != svc::InvalidHandle) { svc::CloseHandle(dbg); }
    }

}
