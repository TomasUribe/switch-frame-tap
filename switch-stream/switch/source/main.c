/* switch-stream sysmodule - reads grc:d video+audio and ships it to a PC.
 *
 * Install:
 *   sdmc:/atmosphere/contents/0100000000000B00/exefs.nsp
 *   sdmc:/atmosphere/contents/0100000000000B00/flags/boot2.flag   (empty)
 * Config:
 *   sdmc:/config/switch-stream/mode.txt   ->  "usb" | "tcp" | "tcp:<port>"
 */
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "proto.h"
#include "capture.h"
#include "transport.h"

/* ---- sysmodule runtime configuration ---------------------------------- */
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

#define INNER_HEAP_SIZE 0x200000
char   __nx_inner_heap[INNER_HEAP_SIZE];
size_t __nx_inner_heap_size = INNER_HEAP_SIZE;

void __libnx_initheap(void)
{
    extern char *fake_heap_start;
    extern char *fake_heap_end;
    fake_heap_start = __nx_inner_heap;
    fake_heap_end   = __nx_inner_heap + INNER_HEAP_SIZE;
}

static char g_mode[32] = "usb";

static void read_mode_file(void)
{
    FILE *f = fopen("sdmc:/config/switch-stream/mode.txt", "r");
    if (!f) return;
    char line[32] = {0};
    if (fgets(line, sizeof(line), f)) {
        /* trim */
        size_t n = strlen(line);
        while (n && (isspace((unsigned char)line[n - 1]))) line[--n] = 0;
        size_t s = 0;
        while (line[s] && isspace((unsigned char)line[s])) s++;
        if (line[s]) { strncpy(g_mode, line + s, sizeof(g_mode) - 1); g_mode[sizeof(g_mode)-1]=0; }
    }
    fclose(f);
}

void __appInit(void)
{
    Result rc;

    rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(rc);

    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(rc);
    fsdevMountSdmc();

    read_mode_file();

    if (strncmp(g_mode, "tcp", 3) == 0) {
        rc = socketInitializeDefault();
        if (R_FAILED(rc)) diagAbortWithResult(rc);
    }

    rc = cap_init();
    if (R_FAILED(rc)) diagAbortWithResult(rc);
}

void __appExit(void)
{
    cap_exit();
    if (strncmp(g_mode, "tcp", 3) == 0) socketExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

/* ---- worker threads -------------------------------------------------- */

static uint8_t g_vbuf[CAP_VIDEO_BUF];
static uint8_t g_abuf[CAP_AUDIO_BUF];
static bool    g_running = true;

static void video_thread(void *arg)
{
    (void)arg;
    while (g_running) {
        uint64_t ts = 0;
        bool key = false;
        int n = cap_read_video(g_vbuf, sizeof(g_vbuf), &ts, &key);
        if (n <= 0) { svcSleepThread(2000000ULL); continue; }   /* 2 ms */

        if (!tr_connected()) {
            /* Block here until a receiver attaches. The audio thread keeps
             * draining grc:d meanwhile so the encoder doesn't stall. */
            if (tr_wait_client() != 0) { svcSleepThread(100000000ULL); continue; }
        }

        if (key) tr_send_hello();
        tr_send(SW_PKT_VIDEO, key ? SW_FLAG_KEYFRAME : 0, ts, g_vbuf, (uint32_t)n);
    }
}

static void audio_thread(void *arg)
{
    (void)arg;
    while (g_running) {
        uint64_t ts = 0;
        int n = cap_read_audio(g_abuf, sizeof(g_abuf), &ts);
        if (n <= 0) { svcSleepThread(2000000ULL); continue; }
        if (tr_connected())
            tr_send(SW_PKT_AUDIO, 0, ts, g_abuf, (uint32_t)n);
    }
}

int main(void)
{
    if (tr_init(g_mode) != 0)
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadInput));

    Thread vt, at;
    /* core 3 = system-reserved core; keep game cores (0-2) untouched. */
    threadCreate(&vt, video_thread, NULL, NULL, 0x8000, 0x2C, 3);
    threadCreate(&at, audio_thread, NULL, NULL, 0x4000, 0x2D, 3);
    threadStart(&vt);
    threadStart(&at);

    while (g_running)
        svcSleepThread(1000000000ULL);   /* 1 s */

    threadWaitForExit(&vt);
    threadWaitForExit(&at);
    threadClose(&vt);
    threadClose(&at);
    tr_exit();
    return 0;
}
