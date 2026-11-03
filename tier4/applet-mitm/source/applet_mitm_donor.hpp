/*
 * v0.7.10: streaming memory lent by a helper program (tier4/donor).
 *
 * The browser applets need nearly the whole Applet pool (2210-1024 with any
 * sysmodule resident there), and streaming needs ~42 MB only that pool has.
 * While a viewer is connected, the module launches sft-donor (0100000000000C21)
 * through pm:shell; it takes the memory in the Applet pool and hands it over
 * as transfer memory (sftap command 10). Terminating the donor gives it back.
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    /* Launches the donor and maps its memory; true with the region on success.
     * Blocks up to a few seconds. Not for IPC handler threads. */
    bool DonorObtain(uintptr_t *out_addr, size_t *out_size);
    /* Unmaps the region and terminates the donor. */
    void DonorRelease();
    bool DonorActive();
    /* An applet that needs the pool is open (the browser: manuals, news,
     * eShop) - the donor must not take its memory now. out_mb: Applet group
     * memory in use. */
    bool AppletBusy(u64 *out_mb = nullptr);
    /* Album title 010000000000100D is up - stock photoViewer, hbmenu, or SwitchU. */
    bool AlbumOrSwitchUOpen();
    /* v0.7.15: what the viewer is told while AppletBusy holds the stream:
     * the keyboard has its own message (beside System Settings it always
     * pauses the stream) */
    bool KeyboardHoldsStream();
    /* v0.7.18: the donor was taken smaller (beside the Album or System
     * Settings) and the Applet pool has room for the full one again - true at
     * most once a minute, logged; the stream then sets up again on it */
    bool DonorCanGrow();

    /* Set when an applet starts while the donor's memory is in use: the
     * stream stops and the memory goes back at once (the browser's big
     * allocation follows its start by ~0.5-3 s). Cleared by the next setup. */
    extern std::atomic<bool> g_applet_needs_memory;
    void StartAppletWatch();
    /* v0.7.10: am is about to start a browser applet (applet_mitm_am.cpp):
     * stop the stream and give the memory back first; no donor for a few
     * seconds after, so the applet gets its share. Blocks up to ~1.5 s. */
    void PrepareForApplet(u32 applet_id);
    /* exp/detach-on-home: game stopped presenting (HOME / SwitchU). Force the
     * stream to give the donor's memory back immediately and hold engine
     * re-setup for a few seconds so Album is not racing a 42 MB remount. */
    void PrepareForHomeMenu();
    /* v0.7.16: a web applet (browser, eShop, manuals, OfflineWeb) is running */
    bool WebAppletOpen();
    bool AppletHoldActive();
    /* Present stall alone is not HOME (loads pause queueBuffer too). Detach when
     * HOME was pressed (stall >= 50 ms), or stall >= probe_ms with Album /
     * applet confirm, or stall >= fallback_ms. out_reason may be null. */
    bool HomeDetachShouldFire(u64 stall_age_ms, u32 probe_ms, u32 fallback_ms, const char **out_reason);

    /* sftap commands 10 / 11, from the donor */
    void DonorAccept(u64 size, os::NativeHandle tmem);
    /* v0.7.13: command 12 also brings nvdrv's transfer memory; the engines
     * take it (and then own the handle) */
    void DonorAcceptNv(u64 size, os::NativeHandle tmem);
    bool DonorTakeNvTmem(os::NativeHandle *out, u64 *out_size);
    void DonorFailed(u32 rc);


    /* v0.7.14: the donor asks this before it takes its heap (sftap 13) */
    u32 DonorCapMb();

}
