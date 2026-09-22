/*
 * applet-mitm - M72: the grc recorder. See applet_mitm_nvdrv.hpp for the why.
 *
 * Record stream (sdmc:/nvenc-grc.bin), little-endian, decoded by tools/nvrec.py:
 *   RecHdr (32 B) + payload[len]
 */
#include "applet_mitm_nvdrv.hpp"
#include "applet_mitm_log.hpp"
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <algorithm>

namespace ams::mitm::applet {

    constinit bool g_grc_armed = false;

    namespace {

        enum RecKind : u16 {
            Rec_Open = 1, Rec_IoctlIn = 2, Rec_IoctlOut = 3, Rec_Ioctl2In = 4, Rec_Ioctl3In = 5,
            Rec_Close = 6, Rec_Cmdbuf = 7, Rec_Setup = 8, Rec_Note = 9,
        };

        struct RecHdr {
            u32 magic;      /* 'NVRC' */
            u16 kind;
            u16 hdr_len;
            u32 fd;
            u32 rq;
            u32 a;
            u32 b;
            u32 len;
            u32 ms;
        };
        static_assert(sizeof(RecHdr) == 32);

        constexpr const char *RecPath = "sdmc:/nvenc-grc.bin";
        constexpr size_t RecCap = 256 * 1024;
        constexpr u32 MaxDumpedSubmits = 8;
        constexpr size_t MaxIoctlBytes = 512;

        alignas(0x1000) constinit u8 g_rec[RecCap];
        constinit size_t g_rec_len     = 0;   /* bytes recorded           */
        constinit size_t g_rec_flushed = 0;   /* bytes already on the SD  */
        constinit bool   g_rec_full    = false;
        constinit bool   g_rec_created = false;
        constinit os::SdkMutex g_rec_lock;

        /* diagnostics the heartbeat reports */
        constinit std::atomic<u32> g_n_ioctl  = 0;
        constinit std::atomic<u32> g_n_submit = 0;
        constinit std::atomic<u32> g_n_dumped = 0;
        constinit std::atomic<u32> g_n_sess   = 0;

        constexpr size_t MaxQueries = 24;
        constinit u64 g_queries[MaxQueries] = {};
        constinit std::atomic<u32> g_nq = 0;
        constinit u32 g_nq_logged = 0;

        /* debug-read scratch, guarded by its own lock (Record() takes
         * g_rec_lock itself, so the two never nest the other way round) */
        alignas(0x1000) constinit u8 g_scratch[0x4000];
        constinit os::SdkMutex g_scratch_lock;

        void Record(RecKind kind, u32 fd, u32 rq, u32 a, u32 b, const void *p, size_t len) {
            std::scoped_lock lk(g_rec_lock);
            if (g_rec_full) { return; }
            if (g_rec_len + sizeof(RecHdr) + len > RecCap) { g_rec_full = true; return; }
            const RecHdr h = {
                0x4352564Eu, static_cast<u16>(kind), sizeof(RecHdr), fd, rq, a, b,
                static_cast<u32>(len),
                static_cast<u32>(armTicksToNs(armGetSystemTick()) / UINT64_C(1000000)),
            };
            std::memcpy(g_rec + g_rec_len, std::addressof(h), sizeof(h));
            if (len != 0) { std::memcpy(g_rec + g_rec_len + sizeof(h), p, len); }
            g_rec_len += sizeof(h) + len;
        }

        void Note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
        void Note(const char *fmt, ...) {
            char b[192];
            va_list vl; va_start(vl, fmt);
            const int n = std::vsnprintf(b, sizeof(b), fmt, vl);
            va_end(vl);
            if (n > 0) { Record(Rec_Note, 0, 0, 0, 0, b, std::min<size_t>(n, sizeof(b) - 1)); }
        }

        constexpr u32 IocNr(u32 rq)   { return rq & 0xFF; }
        constexpr u32 IocType(u32 rq) { return (rq >> 8) & 0xFF; }

        /* A short attach: suspend grc, drain, resume, read, detach. grc is
         * blocked inside our IPC handler for the whole of it anyway. Closing
         * the handle detaches, so nothing is left attached between submits. */
        class GrcReader {
            public:
                explicit GrcReader(u64 pid) {
                    m_rc = ::ams::svc::DebugActiveProcess(std::addressof(m_dbg), pid);
                    if (R_FAILED(m_rc)) { m_dbg = ::ams::svc::InvalidHandle; return; }
                    ::ams::svc::DebugEventInfo ev;
                    u32 n = 0;
                    while (n < 256 && R_SUCCEEDED(::ams::svc::GetDebugEvent(std::addressof(ev), m_dbg))) { ++n; }
                    m_cont = ::ams::svc::ContinueDebugEvent(m_dbg,
                                 ::ams::svc::ContinueFlag_ExceptionHandled | ::ams::svc::ContinueFlag_ContinueAll,
                                 nullptr, 0);
                }
                ~GrcReader() { if (m_dbg != ::ams::svc::InvalidHandle) { static_cast<void>(::ams::svc::CloseHandle(m_dbg)); } }
                bool Ok() const { return m_dbg != ::ams::svc::InvalidHandle; }
                u32 AttachRc() const { return m_rc.GetValue(); }
                u32 ContRc() const { return m_cont.GetValue(); }
                bool Read(u64 va, void *dst, size_t len) {
                    if (!this->Ok() || va == 0) { return false; }
                    return R_SUCCEEDED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(dst), m_dbg, va, len));
                }
            private:
                ::ams::svc::Handle m_dbg = ::ams::svc::InvalidHandle;
                Result m_rc   = ResultSuccess();
                Result m_cont = ResultSuccess();
        };

    }

    void NoteNvdrvQuery(u64 program_id) {
        const u32 i = g_nq.fetch_add(1);
        if (i < MaxQueries) { g_queries[i] = program_id; }
    }

    NvDrvMitm::Hnd *NvDrvMitm::FindHandle(u32 h, bool create) {
        for (u32 i = 0; i < m_nh; ++i) { if (m_h[i].handle == h) { return std::addressof(m_h[i]); } }
        if (!create || m_nh >= MaxHandles) { return nullptr; }
        m_h[m_nh] = { h, 0, 0, 0 };
        return std::addressof(m_h[m_nh++]);
    }

    Result NvDrvMitm::Open(sf::Out<u32> out_fd, sf::Out<u32> out_err, const sf::InBuffer &path) {
        if (g_n_sess.load() == 0) { g_n_sess = 1; }
        struct { u32 fd; u32 err; } o = {};
        const Result rc = serviceDispatchOut(m_forward_service.get(), 0, o,
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
            .buffers      = { { path.GetPointer(), path.GetSize() } },
        );
        R_TRY(rc);

        char p[64] = {};
        std::memcpy(p, path.GetPointer(), std::min<size_t>(path.GetSize(), sizeof(p) - 1));
        FdKind k = Fd_Other;
        if (std::strcmp(p, "/dev/nvmap") == 0)             { k = Fd_Nvmap; }
        else if (std::strcmp(p, "/dev/nvhost-msenc") == 0) { k = Fd_Msenc; }
        else if (std::strcmp(p, "/dev/nvhost-ctrl") == 0)  { k = Fd_Ctrl;  }
        if (o.fd < MaxFds) { m_fd_kind[o.fd] = k; }
        Record(Rec_Open, o.fd, 0, o.fd, o.err, p, std::strlen(p));

        out_fd.SetValue(o.fd);
        out_err.SetValue(o.err);
        R_SUCCEED();
    }

    void NvDrvMitm::DumpSubmit(const u8 *in, size_t len, u32 fd) {
        if (len < 16) { return; }
        u32 hdr[4];
        std::memcpy(hdr, in, sizeof(hdr));
        const u32 ncb = hdr[0];
        if (ncb == 0 || ncb > 8 || 16 + ncb * 12 > len) { Note("submit: implausible num_cmdbufs=%u len=%zu", ncb, len); return; }

        /* g_scratch is shared by every grc session; this lock guards only it */
        std::scoped_lock lk(g_scratch_lock);
        GrcReader rd(m_client_info.process_id.value);
        Note("submit#%u attach rc=0x%x continue rc=0x%x", g_n_submit.load(), rd.AttachRc(), rd.ContRc());
        if (!rd.Ok()) { return; }

        for (u32 c = 0; c < ncb; ++c) {
            u32 cb[3];
            std::memcpy(cb, in + 16 + c * 12, sizeof(cb));
            const u32 mem = cb[0], off = cb[1];
            const u32 words = std::min<u32>(cb[2], sizeof(g_scratch) / 4);
            const Hnd *h = this->FindHandle(mem, false);
            const u64 va = (h != nullptr && h->addr != 0) ? h->addr + off : 0;

            if (!rd.Read(va, g_scratch, words * 4)) {
                Note("cmdbuf %u: handle 0x%x off 0x%x words %u va 0x%llx - UNREADABLE (%s)", c, mem, off, cb[2],
                     static_cast<unsigned long long>(va), h == nullptr ? "handle never allocated in this session" : "read failed");
                continue;
            }
            Record(Rec_Cmdbuf, fd, 0, mem, off, g_scratch, words * 4);

            /* Replay the register writes just far enough to find the pointer to
             * drv_pic_setup (method 0x710, SET_IN_DRV_PIC_SETUP), so the struct
             * itself can be read while grc is still waiting on us. */
            const u32 *w = reinterpret_cast<const u32 *>(g_scratch);
            u32 method = 0, setup_iova = 0;
            auto reg = [&](u32 r, u32 v) {
                if (r == 0x10) { method = v; }
                else if (r == 0x11 && (method << 2) == 0x710) { setup_iova = v << 8; }
            };
            for (u32 i = 0; i < words; ) {
                const u32 op = w[i] >> 28, o = (w[i] >> 16) & 0xFFF, n = w[i] & 0xFFFF;
                ++i;
                if (op == 1)      { for (u32 j = 0; j < n && i < words; ++j) { reg(o + j, w[i++]); } }
                else if (op == 2) { for (u32 j = 0; j < n && i < words; ++j) { reg(o, w[i++]); } }
                else if (op == 3) { for (u32 bit = 0; bit < 16 && i < words; ++bit) { if (n & (1u << bit)) { reg(o + bit, w[i++]); } } }
                else if (op == 4) { reg(o, n); }
            }

            if (setup_iova == 0) { Note("cmdbuf %u: no write to 0x710 found in %u words", c, words); continue; }

            const Hnd *sh = nullptr;
            for (u32 k = 0; k < m_nh; ++k) {
                if (m_h[k].iova != 0 && setup_iova >= m_h[k].iova && setup_iova - m_h[k].iova < m_h[k].size) { sh = std::addressof(m_h[k]); break; }
            }
            const u64 sva = (sh != nullptr && sh->addr != 0) ? sh->addr + (setup_iova - sh->iova) : 0;
            if (rd.Read(sva, g_scratch, 0x1000)) {
                Record(Rec_Setup, fd, 0, setup_iova, static_cast<u32>(sva), g_scratch, 0x1000);
            } else {
                Note("setup iova 0x%x -> va 0x%llx UNREADABLE (%s)", setup_iova, static_cast<unsigned long long>(sva),
                     sh == nullptr ? "no MAP_CMD_BUFFER covers it" : "read failed");
            }
        }
        g_n_dumped.fetch_add(1);
    }

    Result NvDrvMitm::Ioctl(sf::Out<u32> out_err, u32 fd, u32 rq, const sf::InAutoSelectBuffer &in, const sf::OutAutoSelectBuffer &out) {
        g_n_ioctl.fetch_add(1);
        const FdKind k = this->KindOf(fd);
        const u8 *ip = static_cast<const u8 *>(in.GetPointer());
        const size_t il = in.GetSize();

        /* Everything that is not the encoder or a buffer allocation goes
         * straight through, untouched and unrecorded. */
        const bool nvmap_track = (k == Fd_Nvmap) && (IocNr(rq) == 0x01 || IocNr(rq) == 0x04);
        if (k != Fd_Msenc && !nvmap_track) {
            R_THROW(sm::mitm::ResultShouldForwardToSession());
        }

        Record(Rec_IoctlIn, fd, rq, static_cast<u32>(il), static_cast<u32>(out.GetSize()), ip, std::min(il, MaxIoctlBytes));

        if (k == Fd_Msenc && IocType(rq) == 0x00 && IocNr(rq) == 0x01) {
            const u32 s = g_n_submit.fetch_add(1);
            if (s < MaxDumpedSubmits) { this->DumpSubmit(ip, il, fd); }
        }

        struct { u32 fd; u32 rq; } a = { fd, rq };
        u32 err = 0;
        const Result rc = serviceDispatchInOut(m_forward_service.get(), 1, a, err,
            .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_In, SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out },
            .buffers      = { { in.GetPointer(), in.GetSize() }, { out.GetPointer(), out.GetSize() } },
        );
        R_TRY(rc);

        const u8 *op = static_cast<const u8 *>(out.GetPointer());
        const size_t ol = out.GetSize();
        Record(Rec_IoctlOut, fd, rq, err, 0, op, std::min(ol, MaxIoctlBytes));

        /* bookkeeping for the address translation DumpSubmit needs */
        if (k == Fd_Nvmap && IocNr(rq) == 0x01 && ol >= 8) {            /* CREATE {size, handle} */
            u32 v[2]; std::memcpy(v, op, 8);
            if (Hnd *h = this->FindHandle(v[1], true)) { h->size = v[0]; }
        } else if (k == Fd_Nvmap && IocNr(rq) == 0x04 && il >= 32) {    /* ALLOC {handle,...,u64 addr @24} */
            u32 hv; u64 addr;
            std::memcpy(std::addressof(hv), ip, 4);
            std::memcpy(std::addressof(addr), ip + 24, 8);
            if (Hnd *h = this->FindHandle(hv, true)) { h->addr = addr; }
        } else if (k == Fd_Msenc && (IocNr(rq) == 0x09 || IocNr(rq) == 0x25) && ol >= 12) {  /* MAP_CMD_BUFFER */
            u32 num; std::memcpy(std::addressof(num), op, 4);
            for (u32 i = 0; i < num && 12 + i * 8 + 8 <= ol; ++i) {
                u32 e[2]; std::memcpy(e, op + 12 + i * 8, 8);
                if (Hnd *h = this->FindHandle(e[0], true)) { h->iova = e[1]; }
            }
        }

        out_err.SetValue(err);
        R_SUCCEED();
    }

    Result NvDrvMitm::Close(sf::Out<u32> out_err, u32 fd) {
        static_cast<void>(out_err);
        Record(Rec_Close, fd, 0, this->KindOf(fd), 0, nullptr, 0);
        if (fd < MaxFds) { m_fd_kind[fd] = Fd_Unknown; }
        R_THROW(sm::mitm::ResultShouldForwardToSession());
    }

    Result NvDrvMitm::Ioctl2(sf::Out<u32> out_err, u32 fd, u32 rq, const sf::InAutoSelectBuffer &in, const sf::InAutoSelectBuffer &in2, const sf::OutAutoSelectBuffer &out) {
        static_cast<void>(out_err); static_cast<void>(out);
        if (this->KindOf(fd) == Fd_Msenc) {
            Record(Rec_Ioctl2In, fd, rq, static_cast<u32>(in.GetSize()), static_cast<u32>(in2.GetSize()),
                   in.GetPointer(), std::min(in.GetSize(), MaxIoctlBytes));
        }
        R_THROW(sm::mitm::ResultShouldForwardToSession());
    }

    Result NvDrvMitm::Ioctl3(sf::Out<u32> out_err, u32 fd, u32 rq, const sf::InAutoSelectBuffer &in, const sf::OutAutoSelectBuffer &out, const sf::OutAutoSelectBuffer &out2) {
        static_cast<void>(out_err); static_cast<void>(out2);
        if (this->KindOf(fd) == Fd_Msenc) {
            Record(Rec_Ioctl3In, fd, rq, static_cast<u32>(in.GetSize()), static_cast<u32>(out.GetSize()),
                   in.GetPointer(), std::min(in.GetSize(), MaxIoctlBytes));
        }
        R_THROW(sm::mitm::ResultShouldForwardToSession());
    }

    void FlushNvdrvRecords() {
        /* who asked for nvdrv:s - reported once each, so a miss is diagnosable */
        const u32 nq = std::min<u32>(g_nq.load(), MaxQueries);
        for (; g_nq_logged < nq; ++g_nq_logged) {
            LogLine("   nvdrv:s query #%u from %016llx%s", g_nq_logged,
                    static_cast<unsigned long long>(g_queries[g_nq_logged]),
                    g_queries[g_nq_logged] == GrcProgramId ? "  <- grc, INTERCEPTED" : "");
        }

        std::scoped_lock lk(g_rec_lock);
        if (g_rec_len == g_rec_flushed) { return; }
        if (!g_rec_created) {
            fs::DeleteFile(RecPath);
            if (R_FAILED(fs::CreateFile(RecPath, 0))) { return; }
            g_rec_created = true;
        }
        fs::FileHandle f;
        if (R_FAILED(fs::OpenFile(std::addressof(f), RecPath, fs::OpenMode_Write | fs::OpenMode_AllowAppend))) { return; }
        static_cast<void>(fs::WriteFile(f, static_cast<s64>(g_rec_flushed), g_rec + g_rec_flushed,
                                        g_rec_len - g_rec_flushed, fs::WriteOption::Flush));
        fs::CloseFile(f);
        g_rec_flushed = g_rec_len;
        LogLine("   grc recorder: ioctls=%u msenc_submits=%u dumped=%u recorded=%zu B%s",
                g_n_ioctl.load(), g_n_submit.load(), g_n_dumped.load(), g_rec_len,
                g_rec_full ? " (FULL - recording stopped)" : "");
    }

}
