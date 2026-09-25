/*
 * applet-mitm - M84: the debug event pump. See applet_mitm_dbgpump.hpp.
 *
 * Threads:
 *   Pump (core 3)    - blocks on the debug handle with a 50 ms timeout (the
 *                      timeout is only how quickly Stop is noticed), drains,
 *                      then continues - itself, or through a helper when the
 *                      last new thread sits on another core.
 *   Helper (0, 1, 2) - each blocks on its own queue, runs one
 *                      ContinueDebugEvent, and hands back the result.
 *
 * The pump thread never logs. A pending event holds every game thread
 * suspended, so nothing slow may sit between the wake and the continue; the
 * counters are read and logged by whoever owns the stream.
 *
 * The pump runs one notch above the IPC thread (svc 48 vs 49). It is never
 * runnable for long: every path blocks or returns within microseconds, and a
 * handle that stays signalled with nothing to drain (the game exited) makes
 * the pump stop, not spin - a spin at this priority would starve the IPC
 * thread on core 3 (the M60 trap).
 */
#include "applet_mitm_dbgpump.hpp"
#include "applet_mitm_log.hpp"
#include <atomic>

namespace ams::mitm::applet {

    namespace {

        /* os priorities are svc - 28: 20 is svc 48, main/IPC is svc 49 */
        constexpr s32 PumpPriority   = 20;
        constexpr s32 HelperPriority = 20;
        constexpr s64 WaitTimeoutNs  = 50'000'000;
        constexpr u32 PumpCore       = 3;
        constexpr u32 NumHelpers     = 3;   /* cores 0, 1, 2 */

        alignas(os::ThreadStackAlignment) constinit u8 g_pump_stack[0x2000];
        alignas(os::ThreadStackAlignment) constinit u8 g_helper_stack[NumHelpers][0x1000];
        constinit os::ThreadType g_pump_thread;
        constinit os::ThreadType g_helper_thread[NumHelpers];

        constinit os::EventType g_start_event;   /* autoclear: Start -> pump */
        constinit os::EventType g_idle_event;    /* autoclear: pump -> Stop, once per Start */
        constinit os::MessageQueueType g_req_mq[NumHelpers];
        constinit os::MessageQueueType g_res_mq[NumHelpers];
        constinit uintptr_t g_req_buf[NumHelpers][1];
        constinit uintptr_t g_res_buf[NumHelpers][1];

        constinit bool g_initialized = false;
        constinit bool g_pump_ok = false;
        constinit bool g_started = false;        /* owner thread only */
        constinit bool g_helper_ok[NumHelpers] = {};
        constinit ::ams::svc::Handle g_handle = ::ams::svc::InvalidHandle;
        constinit std::atomic<bool> g_stop{false};
        constinit std::atomic<bool> g_gone{false};

        constinit os::SdkMutex g_stats_lock;
        constinit DebugPumpStats g_pump_stats = {};

        Result Continue(::ams::svc::Handle h) {
            R_RETURN(::ams::svc::ContinueDebugEvent(h,
                     ::ams::svc::ContinueFlag_ExceptionHandled | ::ams::svc::ContinueFlag_ContinueAll,
                     nullptr, 0));
        }

        void HelperThread(void *arg) {
            const u32 i = static_cast<u32>(reinterpret_cast<uintptr_t>(arg));
            for (;;) {
                uintptr_t h = 0;
                os::ReceiveMessageQueue(std::addressof(h), std::addressof(g_req_mq[i]));
                const Result rc = Continue(static_cast<::ams::svc::Handle>(h));
                os::SendMessageQueue(std::addressof(g_res_mq[i]), static_cast<uintptr_t>(rc.GetValue()));
            }
        }

        /* One wake: drain, continue. False when the game is gone. */
        bool Service(::ams::svc::Handle h) {
            const u64 t0 = armTicksToNs(armGetSystemTick());
            u32 n = 0, created = 0, exited = 0, other = 0;
            u32 core = PumpCore;
            bool process_exited = false;
            ::ams::svc::DebugEventInfo ev;
            while (n < 256 && R_SUCCEEDED(::ams::svc::GetDebugEvent(std::addressof(ev), h))) {
                ++n;
                switch (ev.type) {
                    case ::ams::svc::DebugEvent_CreateThread: {
                        ++created;
                        u64 o64 = 0; u32 o32 = 0;
                        if (R_SUCCEEDED(::ams::svc::GetDebugThreadParam(std::addressof(o64), std::addressof(o32), h,
                                                                       ev.info.create_thread.thread_id,
                                                                       ::ams::svc::DebugThreadParam_CurrentCore))) {
                            core = o32;
                        }
                        break;
                    }
                    case ::ams::svc::DebugEvent_ExitThread:  ++exited; break;
                    case ::ams::svc::DebugEvent_ExitProcess: ++other; process_exited = true; break;
                    default:                                  ++other; break;
                }
            }

            /* Signalled with nothing queued: detached or terminated
             * (KDebugBase::IsSignaled). Nothing to continue. */
            if (n == 0) {
                std::scoped_lock lk(g_stats_lock);
                ++g_pump_stats.wakes;
                return false;
            }

            Result rc;
            u32 ran_on = PumpCore;
            if (core < NumHelpers && g_helper_ok[core]) {
                os::SendMessageQueue(std::addressof(g_req_mq[core]), static_cast<uintptr_t>(h));
                uintptr_t v = 0;
                os::ReceiveMessageQueue(std::addressof(v), std::addressof(g_res_mq[core]));
                rc = ::ams::Result(static_cast<u32>(v));
                ran_on = core;
            } else {
                rc = Continue(h);
            }
            const u64 held = armTicksToNs(armGetSystemTick()) - t0;

            std::scoped_lock lk(g_stats_lock);
            ++g_pump_stats.wakes;
            g_pump_stats.events  += n;
            g_pump_stats.created += created;
            g_pump_stats.exited  += exited;
            g_pump_stats.other   += other;
            if (held > g_pump_stats.max_hold_ns) { g_pump_stats.max_hold_ns = held; }
            if (R_SUCCEEDED(rc)) {
                ++g_pump_stats.continues;
                ++g_pump_stats.per_core[ran_on & 3];
            } else if (::ams::svc::ResultBusy::Includes(rc)) {
                ++g_pump_stats.busy;          /* the handle is still signalled; next wake handles it */
            } else {
                ++g_pump_stats.failed;
                g_pump_stats.last_fail_rc = rc.GetValue();
                if (::ams::svc::ResultProcessTerminated::Includes(rc)) { return false; }
            }
            return !process_exited;
        }

        void PumpThread(void *) {
            for (;;) {
                os::WaitEvent(std::addressof(g_start_event));
                const ::ams::svc::Handle h = g_handle;
                u32 idle_wakes = 0;
                while (!g_stop.load(std::memory_order_acquire)) {
                    s32 idx = -1;
                    const Result rc = ::ams::svc::WaitSynchronization(std::addressof(idx), std::addressof(h), 1, WaitTimeoutNs);
                    if (g_stop.load(std::memory_order_acquire)) { break; }
                    if (::ams::svc::ResultTimedOut::Includes(rc)) { idle_wakes = 0; continue; }
                    if (R_FAILED(rc)) {
                        std::scoped_lock lk(g_stats_lock);
                        ++g_pump_stats.failed;
                        g_pump_stats.last_fail_rc = rc.GetValue();
                        g_gone.store(true, std::memory_order_release);
                        break;
                    }
                    if (!Service(h)) { g_gone.store(true, std::memory_order_release); break; }
                    /* belt and braces against a spin: a thousand back-to-back
                     * wakes without one timeout is not a game making threads */
                    if (++idle_wakes > 1000) { os::SleepThread(TimeSpan::FromMilliSeconds(1)); idle_wakes = 0; }
                }
                {
                    std::scoped_lock lk(g_stats_lock);
                    g_pump_stats.gone = g_gone.load(std::memory_order_acquire);
                }
                os::SignalEvent(std::addressof(g_idle_event));
            }
        }

        /* True if the pump thread exists. Without it the game runs as in Run I:
         * fine until its first thread start or exit. */
        bool Initialize() {
            if (g_initialized) { return g_pump_ok; }
            g_initialized = true;
            os::InitializeEvent(std::addressof(g_start_event), false, os::EventClearMode_AutoClear);
            os::InitializeEvent(std::addressof(g_idle_event), false, os::EventClearMode_AutoClear);
            for (u32 i = 0; i < NumHelpers; ++i) {
                os::InitializeMessageQueue(std::addressof(g_req_mq[i]), g_req_buf[i], 1);
                os::InitializeMessageQueue(std::addressof(g_res_mq[i]), g_res_buf[i], 1);
                /* A failure here (the NPDM not allowing core i) is not fatal:
                 * that core's continues then run on core 3, as before M84. */
                const Result rc = os::CreateThread(std::addressof(g_helper_thread[i]), HelperThread,
                                                   reinterpret_cast<void *>(static_cast<uintptr_t>(i)),
                                                   g_helper_stack[i], sizeof(g_helper_stack[i]), HelperPriority, static_cast<s32>(i));
                g_helper_ok[i] = R_SUCCEEDED(rc);
                if (g_helper_ok[i]) {
                    os::SetThreadNamePointer(std::addressof(g_helper_thread[i]), "applet-mitm.DbgCore");
                    os::StartThread(std::addressof(g_helper_thread[i]));
                } else {
                    LogLine("   debug pump: helper on core %u not created rc=0x%x - its continues run on core 3", i, rc.GetValue());
                }
            }
            const Result rc = os::CreateThread(std::addressof(g_pump_thread), PumpThread, nullptr,
                                               g_pump_stack, sizeof(g_pump_stack), PumpPriority, static_cast<s32>(PumpCore));
            if (R_FAILED(rc)) {
                LogLine("   debug pump: NOT created rc=0x%x - the game will stop at its next thread start/exit", rc.GetValue());
                return false;
            }
            os::SetThreadNamePointer(std::addressof(g_pump_thread), "applet-mitm.DbgPump");
            os::StartThread(std::addressof(g_pump_thread));
            g_pump_ok = true;
            LogLine("   debug pump: running on core 3; helpers on cores 0/1/2: %s/%s/%s",
                    g_helper_ok[0] ? "yes" : "NO", g_helper_ok[1] ? "yes" : "NO", g_helper_ok[2] ? "yes" : "NO");
            return true;
        }

    }

    void DebugPumpStart(::ams::svc::Handle dbg) {
        if (g_started) { return; }
        if (!Initialize()) { return; }
        {
            std::scoped_lock lk(g_stats_lock);
            g_pump_stats = {};
        }
        g_handle = dbg;
        g_gone.store(false, std::memory_order_release);
        g_stop.store(false, std::memory_order_release);
        g_started = true;
        os::SignalEvent(std::addressof(g_start_event));
    }

    void DebugPumpStop() {
        if (!g_started) { return; }
        g_stop.store(true, std::memory_order_release);
        os::WaitEvent(std::addressof(g_idle_event));
        g_started = false;
        g_handle = ::ams::svc::InvalidHandle;
    }

    bool DebugPumpTargetGone() { return g_gone.load(std::memory_order_acquire); }

    void DebugPumpGetStats(DebugPumpStats *out) {
        std::scoped_lock lk(g_stats_lock);
        *out = g_pump_stats;
        out->gone = g_gone.load(std::memory_order_acquire);
    }

    void DebugPumpLogStats(const char *who) {
        DebugPumpStats s;
        DebugPumpGetStats(std::addressof(s));
        LogLine("   %s: debug events %u (+%u threads, -%u threads, %u other) in %u wakes; continued %u (cores 0-3: %u/%u/%u/%u), busy %u, failed %u (last rc 0x%x); longest hold %llu us%s",
                who, s.events, s.created, s.exited, s.other, s.wakes, s.continues,
                s.per_core[0], s.per_core[1], s.per_core[2], s.per_core[3], s.busy, s.failed, s.last_fail_rc,
                static_cast<unsigned long long>(s.max_hold_ns / 1000), s.gone ? "; GAME GONE" : "");
    }

}
