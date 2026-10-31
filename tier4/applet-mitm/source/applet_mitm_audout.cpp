#include "applet_mitm_audout.hpp"
#include "applet_mitm_audio.hpp"
#include "applet_mitm_dmdbg.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_sm.hpp"
#include "applet_mitm_nv.hpp"

namespace ams::mitm::applet {

    namespace {

        /* see applet_mitm_am.cpp: forwarded objects close with the wrapper,
         * and an out-object on a non-domain session gets its forward service
         * through the tier4 libstratosphere side channel */
        std::shared_ptr<::Service> MakeForward(const ::Service &s) {
            return std::shared_ptr<::Service>(new ::Service(s), [](::Service *p) { serviceClose(p); delete p; });
        }

        void PendForward(const std::shared_ptr<::Service> &fwd) {
            if (!serviceIsDomain(fwd.get()) && !serviceIsDomainSubservice(fwd.get())) {
                ::ams::sf::impl::g_tier4_pending_mitm_forward = fwd;
            }
        }

        enum PortIndex {
            PortIndex_AudOut,
            PortIndex_Count,
        };

        constexpr sm::ServiceName AudOutServiceName = sm::ServiceName::Encode("audout:u");

        struct AudOutServerOptions {
            static constexpr size_t PointerBufferSize   = 0x1000;
            static constexpr size_t MaxDomains          = 0;
            static constexpr size_t MaxDomainObjects    = 0;
            static constexpr bool CanDeferInvokeRequest = false;
            static constexpr bool CanManageMitmServers  = true;
        };
        /* a game's audout:u session and the IAudioOut sessions it opens */
        constexpr size_t AudOutMaxSessions = 8;

        class AudOutServerManager final : public sf::hipc::ServerManager<PortIndex_Count, AudOutServerOptions, AudOutMaxSessions> {
            private:
                virtual Result OnNeedsToAccept(int port_index, Server *server) override;
        };

        AudOutServerManager g_audout_server_manager;

        alignas(os::ThreadStackAlignment) constinit u8 g_audout_stack[8_KB];
        constinit os::ThreadType g_audout_thread;
        void AudOutLoopThread(void *) { LoopProcessSafely(g_audout_server_manager, "audout"); }

        Result AudOutServerManager::OnNeedsToAccept(int port_index, Server *server) {
            std::shared_ptr<::Service> fsrv;
            sm::MitmProcessInfo client_info;
            server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(client_info));
            AMS_ABORT_UNLESS(port_index == PortIndex_AudOut);
            LogLine("audout: program %016llx's audout:u wrapped", static_cast<unsigned long long>(client_info.program_id.value));
            R_RETURN(this->AcceptMitmImpl(server, sf::CreateSharedObjectEmplaced<IAudOutMgrMitm, AudOutMgrMitm>(decltype(fsrv)(fsrv), client_info), fsrv));
        }

        /* libnx's AudioOutBuffer, nn::audio::AudioOutBuffer */
        struct AudOutBuffer {
            u64 next;
            u64 buffer;
            u64 buffer_size;
            u64 data_size;
            u64 data_offset;
        };
        static_assert(sizeof(AudOutBuffer) == 0x28);

        /* one buffer is read at a time (one server thread) */
        alignas(0x1000) constinit u8 g_audout_scratch[0x1000];

    }

    /* v0.7.20 (#57): a game's audio out goes through here only while a viewer
     * is connected - every buffer it appends is one more IPC hop through this
     * process, and with no viewer nothing is taken from it (a real game's
     * sound comes from grc:d anyway). Homebrew always: its sound may have no
     * other source. A viewer that connects later gets grc:d's sound. */
    bool AudOutMgrMitm::ShouldMitm(const sm::MitmProcessInfo &client_info) {
        if (!g_audio_enabled.load(std::memory_order_relaxed) || !ncm::IsApplicationId(client_info.program_id)) { return false; }
        const u32 st = g_live_state.load(std::memory_order_relaxed);
        return client_info.override_status.IsHbl() || st == LiveState_WaitGame || st == LiveState_Streaming;
    }

    Result AudOutMgrMitm::Open(u32 cmd, sf::Out<AudOutInfo> &out_info, sf::Out<sf::SharedPointer<IAudOutObjMitm>> &out, const void *name_in, size_t name_in_size, void *name_out, size_t name_out_size, const AudOutParam &param, u64 aruid, sf::CopyHandle &process) {
        const struct { AudOutParam param; u64 aruid; } in = { param, aruid };
        const u32 attr = cmd == 1 ? SfBufferAttr_HipcMapAlias : SfBufferAttr_HipcAutoSelect;
        AudOutInfo info = {};
        ::Service obj = {};
        R_TRY(serviceMitmDispatchInOut(m_forward_service.get(), cmd, in, info,
            .buffer_attrs    = { attr | SfBufferAttr_In, attr | SfBufferAttr_Out },
            .buffers         = { { name_in, name_in_size }, { name_out, name_out_size } },
            .in_send_pid     = true,
            .in_num_handles  = 1,
            .in_handles      = { process.GetOsHandle() },
            .out_num_objects = 1,
            .out_objects     = std::addressof(obj),
            .override_pid    = m_client_info.process_id.value,
        ));
        out_info.SetValue(info);
        LogLine("audout: program %016llx opened an audio out - %u Hz, %u channels, format %u", static_cast<unsigned long long>(m_client_info.program_id.value),
                info.sample_rate, info.channel_count, info.sample_format);
        const sf::cmif::DomainObjectId target{serviceGetObjectId(std::addressof(obj))};
        auto fwd = MakeForward(obj);
        PendForward(fwd);
        out.SetValue(sf::CreateSharedObjectEmplaced<IAudOutObjMitm, AudOutObjMitm>(decltype(fwd)(fwd), m_client_info, info.channel_count), target);
        R_SUCCEED();
    }

    Result AudOutMgrMitm::OpenAudioOut(sf::Out<AudOutInfo> out_info, sf::Out<sf::SharedPointer<IAudOutObjMitm>> out, const sf::InBuffer &name_in, const sf::OutBuffer &name_out, AudOutParam param, const sf::ClientAppletResourceUserId &aruid, sf::CopyHandle &&process) {
        R_RETURN(this->Open(1, out_info, out, name_in.GetPointer(), name_in.GetSize(), name_out.GetPointer(), name_out.GetSize(), param, aruid.GetValue().value, process));
    }

    Result AudOutMgrMitm::OpenAudioOutAuto(sf::Out<AudOutInfo> out_info, sf::Out<sf::SharedPointer<IAudOutObjMitm>> out, const sf::InAutoSelectBuffer &name_in, const sf::OutAutoSelectBuffer &name_out, AudOutParam param, const sf::ClientAppletResourceUserId &aruid, sf::CopyHandle &&process) {
        R_RETURN(this->Open(3, out_info, out, name_in.GetPointer(), name_in.GetSize(), name_out.GetPointer(), name_out.GetSize(), param, aruid.GetValue().value, process));
    }

    void AudOutObjMitm::Take(const void *buf, size_t size) {
        if (size < sizeof(AudOutBuffer) || !AudioOutWanted()) { return; }
        AudOutBuffer b;
        std::memcpy(std::addressof(b), buf, sizeof(b));
        if (b.buffer == 0 || b.data_size == 0 || b.data_size > 0x40000) { return; }
        /* read through the stream's debug handle on this very game, if it holds one */
        std::scoped_lock lk(g_app_dbg_lock);
        const u32 dbg = g_app_dbg.load(std::memory_order_acquire);
        if (dbg == 0 || g_app_dbg_pid.load(std::memory_order_relaxed) != m_client_info.process_id.value) { return; }
        const u64 src = b.buffer + b.data_offset;
        for (u64 off = 0; off < b.data_size; off += sizeof(g_audout_scratch)) {
            const size_t n = static_cast<size_t>(b.data_size - off < sizeof(g_audout_scratch) ? b.data_size - off : sizeof(g_audout_scratch));
            if (!DbgRead(static_cast<svc::Handle>(dbg), g_audout_scratch, src + off, n)) { return; }
            AudioOutFeed(g_audout_scratch, n, m_channels);
        }
    }

    Result AudOutObjMitm::AppendAudioOutBuffer(const sf::InBuffer &buf, u64 tag) {
        AMS_UNUSED(tag);
        this->Take(buf.GetPointer(), buf.GetSize());
        R_THROW(sm::mitm::ResultShouldForwardToSession());
    }

    Result AudOutObjMitm::AppendAudioOutBufferAuto(const sf::InAutoSelectBuffer &buf, u64 tag) {
        AMS_UNUSED(tag);
        this->Take(buf.GetPointer(), buf.GetSize());
        R_THROW(sm::mitm::ResultShouldForwardToSession());
    }

    void StartAudOutMitm() {
        /* the audio sysmodule's pointer buffer must fit in ours, or accepting a session aborts */
        {
            os::NativeHandle h = os::InvalidNativeHandle;
            if (const Result rc = sm::GetServiceHandle(std::addressof(h), AudOutServiceName); R_FAILED(rc)) {
                LogLine("audout: audout:u not reachable rc=0x%x - no audout sound", rc.GetValue());
                return;
            }
            ::Service s = {};
            serviceCreate(std::addressof(s), h);
            const u16 pbs = s.pointer_buffer_size;
            serviceClose(std::addressof(s));
            LogLine("audout: audout:u pointer buffer %#x (ours %#zx)", pbs, AudOutServerOptions::PointerBufferSize);
            if (pbs > AudOutServerOptions::PointerBufferSize) { LogLine("audout: pointer buffer too large - not wrapping audout:u"); return; }
        }
        if (const Result rc = g_audout_server_manager.RegisterMitmServer<AudOutMgrMitm>(PortIndex_AudOut, AudOutServiceName); R_FAILED(rc)) {
            LogLine("audout: mitm not registered rc=0x%x", rc.GetValue());
            return;
        }
        if (R_FAILED(os::CreateThread(std::addressof(g_audout_thread), AudOutLoopThread, nullptr, g_audout_stack, sizeof(g_audout_stack),
                                      os::GetThreadPriority(os::GetCurrentThread())))) {
            LogLine("audout: server thread not created");
            return;
        }
        os::SetThreadNamePointer(std::addressof(g_audout_thread), "applet-mitm.AudOut");
        os::StartThread(std::addressof(g_audout_thread));
        NoteThread(std::addressof(g_audout_thread));
        LogLine("audout: audout:u wrapped - a game's own PCM is the stream's sound when nothing else has it");
    }

}
