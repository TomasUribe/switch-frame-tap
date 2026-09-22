/*
 * applet-mitm - M72: the grc recorder.
 *
 * NVENC takes our submits and never completes them (M68-M71), and every probe
 * so far has been blind: the first job that reaches the engine can hang it, so
 * nothing submitted after it says anything. Meanwhile grc - the system's own
 * game recorder, the thing SysDVR reads from - drives this exact engine
 * successfully on this exact firmware.
 *
 * So stop guessing and record it. This is a logging-only mitm on "nvdrv:s",
 * accepted for grc (0100000000000035) and nobody else. It watches grc's opens
 * and ioctls, and on each of its first msenc submits it attaches to grc with the
 * debug SVCs, reads the command buffer words and the drv_pic_setup they point
 * at, and records all of it to sdmc:/nvenc-grc.bin for offline decoding
 * (tools/nvrec.py). Every request is still forwarded unchanged.
 *
 * mitm.lst in our contents directory makes boot2 DeclareFutureMitm("nvdrv:s"),
 * so grc's first session waits for us instead of racing past. The price: every
 * nvdrv:s client blocks until we register, so registration happens first thing
 * in main and ShouldMitm is a single compare.
 */
#pragma once
#include <stratosphere.hpp>

#define AMS_NVDRV_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H,  0, Result, Open,   (sf::Out<u32> out_fd, sf::Out<u32> out_err, const sf::InBuffer &path), (out_fd, out_err, path)) \
    AMS_SF_METHOD_INFO(C, H,  1, Result, Ioctl,  (sf::Out<u32> out_err, u32 fd, u32 rq, const sf::InAutoSelectBuffer &in, const sf::OutAutoSelectBuffer &out), (out_err, fd, rq, in, out)) \
    AMS_SF_METHOD_INFO(C, H,  2, Result, Close,  (sf::Out<u32> out_err, u32 fd), (out_err, fd)) \
    AMS_SF_METHOD_INFO(C, H, 11, Result, Ioctl2, (sf::Out<u32> out_err, u32 fd, u32 rq, const sf::InAutoSelectBuffer &in, const sf::InAutoSelectBuffer &in2, const sf::OutAutoSelectBuffer &out), (out_err, fd, rq, in, in2, out)) \
    AMS_SF_METHOD_INFO(C, H, 12, Result, Ioctl3, (sf::Out<u32> out_err, u32 fd, u32 rq, const sf::InAutoSelectBuffer &in, const sf::OutAutoSelectBuffer &out, const sf::OutAutoSelectBuffer &out2), (out_err, fd, rq, in, out, out2))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, INvDrvMitm, AMS_NVDRV_MITM_INTERFACE_INFO, 0x4E564452)

namespace ams::mitm::applet {

    constexpr u64 GrcProgramId = 0x0100000000000035ull;
    extern bool g_grc_armed;

    /* every nvdrv:s client that asked, so a miss is diagnosable next cycle */
    void NoteNvdrvQuery(u64 program_id);

    class NvDrvMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
                NoteNvdrvQuery(client_info.program_id.value);
                return g_grc_armed && client_info.program_id.value == GrcProgramId;
            }
        private:
            enum FdKind : u8 { Fd_Unknown = 0, Fd_Nvmap, Fd_Msenc, Fd_Ctrl, Fd_Other };
            static constexpr size_t MaxFds = 64, MaxHandles = 256;
            FdKind m_fd_kind[MaxFds] = {};
            struct Hnd { u32 handle, size, iova; u64 addr; };
            Hnd m_h[MaxHandles] = {};
            u32 m_nh = 0;
            Hnd *FindHandle(u32 h, bool create);
            FdKind KindOf(u32 fd) const { return fd < MaxFds ? m_fd_kind[fd] : Fd_Unknown; }
            void DumpSubmit(const u8 *in, size_t len, u32 fd);
        public:
            Result Open(sf::Out<u32> out_fd, sf::Out<u32> out_err, const sf::InBuffer &path);
            Result Ioctl(sf::Out<u32> out_err, u32 fd, u32 rq, const sf::InAutoSelectBuffer &in, const sf::OutAutoSelectBuffer &out);
            Result Close(sf::Out<u32> out_err, u32 fd);
            Result Ioctl2(sf::Out<u32> out_err, u32 fd, u32 rq, const sf::InAutoSelectBuffer &in, const sf::InAutoSelectBuffer &in2, const sf::OutAutoSelectBuffer &out);
            Result Ioctl3(sf::Out<u32> out_err, u32 fd, u32 rq, const sf::InAutoSelectBuffer &in, const sf::OutAutoSelectBuffer &out, const sf::OutAutoSelectBuffer &out2);
    };
    static_assert(IsINvDrvMitm<NvDrvMitm>);

    /* called from the heartbeat thread: appends pending records to SD */
    void FlushNvdrvRecords();

}
