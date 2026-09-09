#include "applet_mitm_log.hpp"
#include <cstdarg>
#include <cstdio>

namespace ams::mitm::applet {

    namespace {

        constexpr const char *LogPath = "sdmc:/applet-mitm.log";
        constinit os::SdkMutex g_lock;
        constinit bool g_ready = false;

    }

    void LogInit() {
        std::scoped_lock lk(g_lock);
        if (R_FAILED(fs::MountSdCard("sdmc"))) {
            return;
        }
        /* truncate */
        fs::DeleteFile(LogPath);
        fs::CreateFile(LogPath, 0);
        g_ready = true;
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

        /* open/append/close every line so a crash can't lose it */
        fs::FileHandle f;
        if (R_FAILED(fs::OpenFile(std::addressof(f), LogPath, fs::OpenMode_Write | fs::OpenMode_AllowAppend))) {
            return;
        }
        s64 size = 0;
        static_cast<void>(fs::GetFileSize(std::addressof(size), f));
        static_cast<void>(fs::WriteFile(f, size, line, n, fs::WriteOption::Flush));
        fs::CloseFile(f);
    }

}
