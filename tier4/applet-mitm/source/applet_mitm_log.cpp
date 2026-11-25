#include "applet_mitm_log.hpp"
#include "applet_mitm_nv.hpp"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace ams::mitm::applet {

    namespace {
        constinit s32 g_thread_base_prio = 21;   /* 49 for the kernel, until main says */
    }
    void SetThreadBasePriority(s32 prio) { g_thread_base_prio = prio; }
    s32 ThreadBasePriority() { return g_thread_base_prio; }

    constinit Stats g_stats = {};

    namespace {

        const char *LogPath  = "sdmc:/applet-mitm.log";
        const char *LastPath = "sdmc:/applet-mitm.last";
        /* M98: a release install logs into its config folder, and stops at
         * 4 MB - a test run's log is read the same day; a user's can run
         * for weeks of boots without anyone looking */
        constexpr s64 ReleaseLogCap = 4 * 1024 * 1024;
        constinit os::SdkMutex g_lock;
        constinit bool g_ready = false;
        /* v0.7.10: held in memory while a teardown races an applet - each
         * flushed line costs the SD ~15 ms, and 20 of them lost the race */
        constinit bool g_defer = false;
        constinit u64 g_defer_tick = 0;   /* v0.7.14: held at most ~3 s - nothing waits forever for its release */
        constinit char g_defer_buf[8192];
        constinit size_t g_defer_len = 0;

        void AppendToLog(const char *data, size_t n) {
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), LogPath, fs::OpenMode_Write | fs::OpenMode_AllowAppend))) { return; }
            s64 size = 0;
            static_cast<void>(fs::GetFileSize(std::addressof(size), f));
            if (g_release_mode && size > ReleaseLogCap) { fs::CloseFile(f); return; }
            static_cast<void>(fs::WriteFile(f, size, data, n, fs::WriteOption::Flush));
            fs::CloseFile(f);
        }

        void WriteWhole(const char *path, const char *data, size_t len) {
            fs::DeleteFile(path);
            if (R_FAILED(fs::CreateFile(path, static_cast<s64>(len)))) { return; }
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), path, fs::OpenMode_Write))) { return; }
            static_cast<void>(fs::WriteFile(f, 0, data, len, fs::WriteOption::Flush));
            fs::CloseFile(f);
        }
        /* v0.7.15 (#33): lines held while a stream ran and dropped (the
         * oldest) once the buffer was full */
        constinit u32 g_held_dropped = 0;

        /* v0.7.15 (#33): a release install writes nothing to the SD card
         * while a stream runs - each flushed write (the heartbeat's every 3 s,
         * the stream's window lines every 20 s) showed as a latency spike in
         * the viewer. Lines wait in g_defer_buf and go out once it stops; a
         * test install (arm file) still writes every line at once. */
        bool StreamQuiet() { return g_release_mode && g_streaming_now.load(std::memory_order_relaxed); }
        /* v0.7.20 (#57): and while a game runs. On emuMMC the SD card is the
         * game's storage, and with no viewer the heartbeat and the memory
         * lines flushed a write about once a second; players saw stutters
         * with the stream off. Held lines go out in one write a minute, when
         * 8 KB are held, or when the game closes. */
        bool GameQuiet() { return g_release_mode && g_app_pid.load(std::memory_order_relaxed) != 0; }
        /* v0.7.24 (#98): config/switch-frame-tap/log-all.txt writes every
         * line at once, streaming or not - a console that froze lost the lines
         * held for later, the ones that say what happened (Dragon Quest XI: the
         * log stops at a restart for a new picture size). The stream may hitch
         * on each write; for finding a freeze, not for playing. */
        constinit bool g_log_all = false;
        bool Quiet() { return !g_log_all && (StreamQuiet() || GameQuiet()); }
        constexpr u64 GameFlushNs = UINT64_C(60000000000);
        constinit u64 g_held_tick = 0;   /* when the oldest held line came in */

        /* room for n more bytes: whole lines go, the oldest first - but the
         * first HeadKeep bytes stay. v0.7.24 (#70): a stream's first lines say
         * what it found (the buffers, the slot match); a 160 s Maestro stream
         * dropped them for the Tesla and memory lines that came after. */
        constexpr size_t HeadKeep = 3072;
        void DropOldest(size_t n) {
            size_t head = 0;
            if (g_defer_len > HeadKeep) {
                const char *nl = static_cast<const char *>(std::memchr(g_defer_buf + HeadKeep, '\n', g_defer_len - HeadKeep));
                head = nl != nullptr ? static_cast<size_t>(nl - g_defer_buf) + 1 : 0;
            }
            if (head + n > sizeof(g_defer_buf) / 2) { head = 0; }
            size_t cut = head;
            while (g_defer_len - (cut - head) + n > sizeof(g_defer_buf) && cut < g_defer_len) {
                const char *nl = static_cast<const char *>(std::memchr(g_defer_buf + cut, '\n', g_defer_len - cut));
                cut = nl != nullptr ? static_cast<size_t>(nl - g_defer_buf) + 1 : g_defer_len;
                ++g_held_dropped;
            }
            std::memmove(g_defer_buf + head, g_defer_buf + cut, g_defer_len - cut);
            g_defer_len -= cut - head;
        }

        void WriteHeld() {
            if (g_held_dropped != 0) {
                char b[96];
                const int n = std::snprintf(b, sizeof(b), "(%u lines from the middle of the stream not kept - the buffer was full)\n", g_held_dropped);
                if (n > 0) { AppendToLog(b, static_cast<size_t>(n)); }
                g_held_dropped = 0;
            }
            if (g_defer_len != 0) { AppendToLog(g_defer_buf, g_defer_len); }
            g_defer_len = 0;
        }

    }

    constinit bool g_release_mode = false;
    constinit u32 g_arm_probe_rc = 0;

    void LogInit() {
        std::scoped_lock lk(g_lock);
        if (R_FAILED(fs::MountSdCard("sdmc"))) {
            return;
        }
        {
            /* logged by Main (LogLine here would take g_lock twice) */
            fs::DirectoryEntryType t;
            const Result r = fs::GetEntryType(std::addressof(t), "sdmc:/applet-mitm.armed");
            g_arm_probe_rc = r.GetValue();
            g_release_mode = R_FAILED(r);
        }
        if (g_release_mode) {
            static_cast<void>(fs::CreateDirectory("sdmc:/config"));
            static_cast<void>(fs::CreateDirectory("sdmc:/config/switch-frame-tap"));
            LogPath  = "sdmc:/config/switch-frame-tap/log.txt";
            LastPath = "sdmc:/config/switch-frame-tap/last.txt";
        }
        /* v0.7.13: the previous boot's log and breadcrumb are kept beside
         * (log.prev.txt, last.prev.txt) - after a crash or a hang the console
         * is rebooted, and the boot that went wrong is the one wanted */
        if (g_release_mode) {
            static_cast<void>(fs::DeleteFile("sdmc:/config/switch-frame-tap/log.prev.txt"));
            static_cast<void>(fs::RenameFile(LogPath, "sdmc:/config/switch-frame-tap/log.prev.txt"));
            static_cast<void>(fs::DeleteFile("sdmc:/config/switch-frame-tap/last.prev.txt"));
            static_cast<void>(fs::RenameFile(LastPath, "sdmc:/config/switch-frame-tap/last.prev.txt"));
        }
        /* truncate */
        fs::DeleteFile(LogPath);
        fs::CreateFile(LogPath, 0);
        {
            fs::DirectoryEntryType t;
            g_log_all = g_release_mode && R_SUCCEEDED(fs::GetEntryType(std::addressof(t), "sdmc:/config/switch-frame-tap/log-all.txt"));
        }
        g_ready = true;
        WriteWhole(LastPath, "LogInit\n", 8);
    }

    void LogMark(const char *what) {
        {
            std::scoped_lock lk(g_lock);
            if (g_ready && !g_defer && !Quiet()) {
                char b[128];
                const int n = std::snprintf(b, sizeof(b), "%s\n", what);
                if (n > 0) { WriteWhole(LastPath, b, static_cast<size_t>(n)); }
            }
        }
        LogLine("-> %s", what);
    }

    void LogLine(const char *fmt, ...) {
        std::scoped_lock lk(g_lock);
        if (!g_ready) {
            return;
        }

        char line[512];
        const u64 ms = armTicksToNs(armGetSystemTick()) / UINT64_C(1000000);
        int n = std::snprintf(line, sizeof(line), "[%6llu.%03llu] ",
                              static_cast<unsigned long long>(ms / 1000),
                              static_cast<unsigned long long>(ms % 1000));

        std::va_list ap;
        va_start(ap, fmt);
        n += std::vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
        va_end(ap);
        if (n < 0) { return; }
        if (static_cast<size_t>(n) > sizeof(line) - 2) { n = sizeof(line) - 2; }
        line[n++] = '\n';

        if (Quiet()) {
            if (g_defer_len + static_cast<size_t>(n) > sizeof(g_defer_buf)) {
                /* a stream must not wait on the card; a game can take one write */
                if (StreamQuiet()) { DropOldest(static_cast<size_t>(n)); } else { WriteHeld(); }
            }
            if (g_defer_len == 0) { g_held_tick = armGetSystemTick(); }
            std::memcpy(g_defer_buf + g_defer_len, line, static_cast<size_t>(n));
            g_defer_len += static_cast<size_t>(n);
            return;
        }
        if (g_defer) {
            /* v0.7.14: held lines go out once the buffer is full or ~3 s on,
             * rather than being dropped or kept for good */
            const bool full = g_defer_len + static_cast<size_t>(n) > sizeof(g_defer_buf);
            if (!full && armTicksToNs(armGetSystemTick() - g_defer_tick) < UINT64_C(3000000000)) {
                std::memcpy(g_defer_buf + g_defer_len, line, static_cast<size_t>(n));
                g_defer_len += static_cast<size_t>(n);
                return;
            }
            g_defer = false;
        }
        WriteHeld();   /* what a stream held, first */
        /* open/append/close every line so a crash can't lose it */
        AppendToLog(line, static_cast<size_t>(n));
    }

    void LogDefer(bool on) {
        std::scoped_lock lk(g_lock);
        if (on && g_log_all) { return; }
        if (on) {
            if (!g_defer) { g_defer_tick = armGetSystemTick(); }
            g_defer = true;
            return;
        }
        g_defer = false;
        if (g_ready && !Quiet()) { WriteHeld(); }
    }

    void LogFlushHeld() {
        std::scoped_lock lk(g_lock);
        if (g_ready && !g_defer && !Quiet()) { WriteHeld(); }
    }

    bool LogQuiet() { return Quiet(); }

    bool LogAll() { return g_log_all; }

    void LogFlushStale() {
        std::scoped_lock lk(g_lock);
        if (g_ready && !g_defer && !StreamQuiet() && g_defer_len != 0 && armTicksToNs(armGetSystemTick() - g_held_tick) >= GameFlushNs) { WriteHeld(); }
    }

    void LogWriteHeldNow() {
        std::scoped_lock lk(g_lock);
        if (g_ready) { WriteHeld(); }
    }


    /* v0.7.24: the viewer's Report a problem button ("SFTG"). config.ini, the
     * log, the lines held in memory, the breadcrumb and the end of the last
     * boot's log go to the viewer a piece at a time, read by the one thread
     * that sends (a stream sends one piece a frame). Nothing is written: a
     * stream that holds its lines keeps holding them. */
    namespace {
        struct ReportPart { const char *title; const char *path; s64 from, len; };   /* path nullptr: the held lines */
        constexpr int MaxReportParts = 6;
        constinit ReportPart g_rp[MaxReportParts] = {};
        constinit int g_rp_n = 0, g_rp_i = 0;
        constinit s64 g_rp_off = 0;
        constinit bool g_rp_titled = false;
        constinit char g_rp_title[2][96] = {};

        /* another thread may have the log open for a line this moment: once more, 2 ms on */
        bool OpenForRead(fs::FileHandle *f, const char *path) {
            if (R_SUCCEEDED(fs::OpenFile(f, path, fs::OpenMode_Read))) { return true; }
            os::SleepThread(TimeSpan::FromMilliSeconds(2));
            return R_SUCCEEDED(fs::OpenFile(f, path, fs::OpenMode_Read));
        }

        s64 SizeOf(const char *path) {
            fs::FileHandle f;
            if (!OpenForRead(std::addressof(f), path)) { return -1; }
            s64 size = 0;
            static_cast<void>(fs::GetFileSize(std::addressof(size), f));
            fs::CloseFile(f);
            return size;
        }

        void ReportAdd(const char *title, const char *path, s64 from, s64 len) {
            if (g_rp_n < MaxReportParts) { g_rp[g_rp_n++] = { title, path, from, len }; }
        }
    }

    void ReportBegin() {
        g_rp_n = 0; g_rp_i = 0; g_rp_off = 0; g_rp_titled = false;
        const char *cfg = "sdmc:/config/switch-frame-tap/config.ini";
        const s64 c = SizeOf(cfg);
        ReportAdd("config.ini", cfg, 0, c < 0 ? -1 : std::min<s64>(c, 8 * 1024));
        /* the start says what this boot found, the end what just happened */
        const s64 l = SizeOf(LogPath);
        constexpr s64 Head = 24 * 1024, Tail = 128 * 1024;
        if (l > Head + Tail) {
            std::snprintf(g_rp_title[0], sizeof(g_rp_title[0]), "log (the first %lld bytes of %lld)", static_cast<long long>(Head), static_cast<long long>(l));
            std::snprintf(g_rp_title[1], sizeof(g_rp_title[1]), "log (the last %lld bytes)", static_cast<long long>(Tail));
            ReportAdd(g_rp_title[0], LogPath, 0, Head);
            ReportAdd(g_rp_title[1], LogPath, l - Tail, Tail);
        } else {
            ReportAdd("log", LogPath, 0, l);
        }
        ReportAdd("lines held in memory (not on the SD card yet)", nullptr, 0, 0);
        ReportAdd("last.txt (the step in flight)", LastPath, 0, std::min<s64>(SizeOf(LastPath), 4 * 1024));
        if (g_release_mode) {
            const char *prev = "sdmc:/config/switch-frame-tap/log.prev.txt";
            const s64 p = SizeOf(prev);
            ReportAdd("log.prev.txt (the end of the last boot's log)", prev, p > 48 * 1024 ? p - 48 * 1024 : 0, std::min<s64>(p, 48 * 1024));
        }
    }

    size_t ReportNext(u8 *out, size_t cap, bool *last) {
        size_t n = 0;
        while (g_rp_i < g_rp_n && n == 0) {
            const ReportPart &p = g_rp[g_rp_i];
            if (!g_rp_titled) {
                const int t = std::snprintf(reinterpret_cast<char *>(out), cap, "\n===== %s =====\n", p.title);
                if (t > 0) { n = static_cast<size_t>(t); }
                g_rp_titled = true;
            }
            bool done = true;
            if (p.path == nullptr) {
                std::scoped_lock lk(g_lock);
                const size_t k = std::min(g_defer_len, cap - n - 1);
                std::memcpy(out + n, g_defer_buf, k);
                n += k;
                if (k == 0) { n += static_cast<size_t>(std::snprintf(reinterpret_cast<char *>(out + n), cap - n, "(none)\n")); }
            } else if (p.len < 0) {
                n += static_cast<size_t>(std::snprintf(reinterpret_cast<char *>(out + n), cap - n, "(not there)\n"));
            } else {
                const s64 want = std::min<s64>(p.len - g_rp_off, static_cast<s64>(cap - n));
                fs::FileHandle f;
                size_t got = 0;
                if (want > 0 && OpenForRead(std::addressof(f), p.path)) {
                    if (R_FAILED(fs::ReadFile(std::addressof(got), f, p.from + g_rp_off, out + n, static_cast<size_t>(want)))) { got = 0; }
                    fs::CloseFile(f);
                }
                n += got;
                g_rp_off += static_cast<s64>(got);
                done = got == 0 || g_rp_off >= p.len;
            }
            if (done) { ++g_rp_i; g_rp_off = 0; g_rp_titled = false; }
        }
        *last = g_rp_i >= g_rp_n;
        if (*last && cap - n > 24) { n += static_cast<size_t>(std::snprintf(reinterpret_cast<char *>(out + n), cap - n, "\n===== end =====\n")); }
        return n;
    }

    namespace {
        constexpr u32 MaxNoted = 32;
        constinit const os::ThreadType *g_noted[MaxNoted] = {};
        constinit u32 g_noted_n = 0;
    }

    void NoteThread(const os::ThreadType *t) {
        std::scoped_lock lk(g_lock);
        for (u32 k = 0; k < g_noted_n; ++k) { if (g_noted[k] == t) { return; } }   /* a thread object made again */
        if (g_noted_n < MaxNoted) { g_noted[g_noted_n++] = t; }
    }

    void LogStackUse() {
        u32 n = 0;
        { std::scoped_lock lk(g_lock); n = g_noted_n; }
        for (u32 k = 0; k < n; ++k) {
            /* the stack where the thread uses it: libnx maps it to a mirror
             * and the original pages become unreadable (test9 read those and
             * fataled the module, Data Abort) */
            const ::Thread *impl = g_noted[k]->thread_impl;
            if (impl == nullptr || impl->stack_mirror == nullptr) { continue; }
            const size_t size = impl->stack_sz;
            const u8 *lo = static_cast<const u8 *>(impl->stack_mirror);
            size_t untouched = 0;
            while (untouched < size && lo[untouched] == 0) { ++untouched; }
            const char *name = os::GetThreadNamePointer(g_noted[k]);
            LogLine("stack: %s used %zu of %zu KB", name != nullptr ? name : "?", (size - untouched + 1023) / 1024, size / 1024);
        }
    }

}
