/*
 * v0.7.17: a game's audout:u - sound for games neither grc:d nor the final
 * output recorder carries.
 *
 * grc:d records a game only once it set up recording (homebrew does not);
 * the final output recorder (audrec:u) carries the application mix of the
 * audio renderer - gen1recomp, Paperboat and Dusklight (SDL2, audren) are
 * heard in it, PokeMMO is silent the whole time at full record volume. Its
 * sound goes out through audout, which hands the audio sysmodule plain PCM
 * buffers in the game's memory (AppendAudioOutBuffer). So the game's audout:u
 * is wrapped: every command goes on to the audio sysmodule unchanged; an
 * appended buffer is read first, through the stream's debug handle on the
 * game (only while a stream runs, which is the only time sound is sent), and
 * goes into the audio ring - when grc:d and the recorder have nothing.
 *
 * Wrapped: audout:u of applications (OpenAudioOut, 1 and the 3.0.0+ Auto 3)
 * and the IAudioOut it returns (AppendAudioOutBuffer 3 / Auto 7). Every other
 * command forwards untouched.
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    struct AudOutParam {
        u32 sample_rate;
        u16 channel_count;
        u16 reserved;
    };
    static_assert(sizeof(AudOutParam) == 8);

    struct AudOutInfo {
        u32 sample_rate;
        u32 channel_count;
        u32 sample_format;
        u32 state;
    };
    static_assert(sizeof(AudOutInfo) == 16);

}

#define AMS_AUDOUT_OBJ_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 3, Result, AppendAudioOutBuffer,     (const sf::InBuffer &buf, u64 tag),           (buf, tag)) \
    AMS_SF_METHOD_INFO(C, H, 7, Result, AppendAudioOutBufferAuto, (const sf::InAutoSelectBuffer &buf, u64 tag), (buf, tag))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IAudOutObjMitm, AMS_AUDOUT_OBJ_MITM_INTERFACE_INFO, 0x41554F31)

#define AMS_AUDOUT_MGR_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 1, Result, OpenAudioOut,     (sf::Out<ams::mitm::applet::AudOutInfo> out_info, sf::Out<sf::SharedPointer<ams::mitm::applet::IAudOutObjMitm>> out, const sf::InBuffer &name_in, const sf::OutBuffer &name_out, ams::mitm::applet::AudOutParam param, const sf::ClientAppletResourceUserId &aruid, sf::CopyHandle &&process), (out_info, out, name_in, name_out, param, aruid, std::move(process))) \
    AMS_SF_METHOD_INFO(C, H, 3, Result, OpenAudioOutAuto, (sf::Out<ams::mitm::applet::AudOutInfo> out_info, sf::Out<sf::SharedPointer<ams::mitm::applet::IAudOutObjMitm>> out, const sf::InAutoSelectBuffer &name_in, const sf::OutAutoSelectBuffer &name_out, ams::mitm::applet::AudOutParam param, const sf::ClientAppletResourceUserId &aruid, sf::CopyHandle &&process), (out_info, out, name_in, name_out, param, aruid, std::move(process)), hos::Version_3_0_0)

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IAudOutMgrMitm, AMS_AUDOUT_MGR_MITM_INTERFACE_INFO, 0x41554F30)

namespace ams::mitm::applet {

    class AudOutObjMitm : public sf::MitmServiceImplBase {
        private:
            u32 m_channels = 2;
        public:
            AudOutObjMitm(std::shared_ptr<::Service> &&s, const sm::MitmProcessInfo &c, u32 channels) : MitmServiceImplBase(std::forward<std::shared_ptr<::Service>>(s), c), m_channels(channels) { /* ... */ }
            static bool ShouldMitm(const sm::MitmProcessInfo &) { return true; }
        private:
            void Take(const void *buf, size_t size);
        public:
            Result AppendAudioOutBuffer(const sf::InBuffer &buf, u64 tag);
            Result AppendAudioOutBufferAuto(const sf::InAutoSelectBuffer &buf, u64 tag);
    };
    static_assert(IsIAudOutObjMitm<AudOutObjMitm>);

    class AudOutMgrMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            /* applications only, and only with sound on; no IPC here (see g_app_pid) */
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info);
        private:
            Result Open(u32 cmd, sf::Out<AudOutInfo> &out_info, sf::Out<sf::SharedPointer<IAudOutObjMitm>> &out, const void *name_in, size_t name_in_size, void *name_out, size_t name_out_size, const AudOutParam &param, u64 aruid, sf::CopyHandle &process);
        public:
            Result OpenAudioOut(sf::Out<AudOutInfo> out_info, sf::Out<sf::SharedPointer<IAudOutObjMitm>> out, const sf::InBuffer &name_in, const sf::OutBuffer &name_out, AudOutParam param, const sf::ClientAppletResourceUserId &aruid, sf::CopyHandle &&process);
            Result OpenAudioOutAuto(sf::Out<AudOutInfo> out_info, sf::Out<sf::SharedPointer<IAudOutObjMitm>> out, const sf::InAutoSelectBuffer &name_in, const sf::OutAutoSelectBuffer &name_out, AudOutParam param, const sf::ClientAppletResourceUserId &aruid, sf::CopyHandle &&process);
    };
    static_assert(IsIAudOutMgrMitm<AudOutMgrMitm>);

    /* main.cpp: register audout:u's mitm on its own server */
    void StartAudOutMitm();

}
