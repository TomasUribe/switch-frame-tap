/*
 * v0.7.14: the HOME menu and the system applets as a stream source.
 *
 * Found in research probes 1-7 (tier4/mitm/STATUS.md, "v0.7.14 research"):
 *  - the HOME menu and the library applets draw into vi's system shared
 *    buffer, 7 slots of 1280x768x4 (1280x720 A8B8G8R8, block-linear, block
 *    height 16) in vi's own .bss; whatever is in front alternates slots 3/4
 *  - each frame is handed to vi with PresentSharedFrameBuffer (fence, layer,
 *    slot); the last such request stays in one of vi's IPC message buffers,
 *    the HOME menu's in its own - so the slot and fence of every HOME-menu
 *    frame can be read out of vi, and nothing has to sit in its IPC path
 *  - attaching to vi as a debugger holds it ~40 us; a slot reads in ~3 ms
 *
 * So a UI session attaches to vi and a feeder thread turns those messages
 * into the same present records a game's queueBuffer makes; the capture
 * (TryNvencStream) then runs unchanged, waiting on each present's fence.
 */
#pragma once
#include <stratosphere.hpp>
#include "applet_mitm_nv.hpp"

namespace ams::mitm::applet {

    /* config.ini home_menu = 1 (arm token "homemenu") */
    extern bool g_ui_stream_armed;

    /* vi's pid, its shared buffer, and that buffer as a 7-slot "swapchain" */
    bool UiSourceFind(u64 *out_pid, u64 *out_base, GameSurface *out_geo);

    /* with vi attached: find the present messages and start the feeder */
    bool UiSourceStart(::ams::svc::Handle dbg, u64 pid, u64 base);
    void UiSourceStop();
    /* v0.7.16: the last UiSourceStart found nothing of the HOME menu drawing */
    bool UiSourceIdle();

    /* one line: presents fed, layers followed, rescans */
    void UiSourceLogStats(const char *who);

}
