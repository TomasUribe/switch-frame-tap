/*
 * v0.3: game audio, from grc:d - the continuous recorder the console keeps for
 * its own 30-second clips (and what SysDVR streams): 16-bit stereo PCM at
 * 48 kHz, read 4 KB (1024 frames, ~21 ms) at a time.
 *
 * A thread of its own reads it, because grcdTransfer blocks - forever, when
 * the running game has video capture disabled - into a ring of ~340 ms. The
 * video stream thread, which owns the USB endpoint, sends whatever has piled
 * up as one SFTR audio packet before each video frame: no two transfers are
 * ever in flight at once. Audio flows only while a stream runs; in between,
 * the ring is emptied so a new stream starts with current sound.
 */
#pragma once
#include <stratosphere.hpp>
#include <atomic>

namespace ams::mitm::applet {

    extern std::atomic<bool> g_audio_enabled;   /* the setting ("audio" token / audio = 1) */
    extern std::atomic<bool> g_audio_active;    /* a live stream is running */
    extern std::atomic<u32>  g_audio_state;     /* 0 idle, 1 running, 2 grc:d unavailable, 3 start failed */

    void StartAudio();

    /* copies up to max bytes of queued PCM (whole 4-byte frames) into dst */
    /* v0.7.13: the ring (AudioRingBytes) is the stream's memory - the capture
     * region's tail, from the donor - attached while a stream runs, not 64 KB
     * of .bss in the System group */
    constexpr size_t AudioRingBytes = 64 * 1024;
    void AudioAttachRing(u8 *ring);
    void AudioDetachRing();
    size_t AudioTake(u8 *dst, size_t max);

    /* drop everything queued (a stream ended, or started) */
    void AudioFlush();

    /* v0.7.17: a game's own audout PCM (applet_mitm_audout.cpp) - wanted only
     * while a stream runs and neither grc:d nor the final output recorder
     * has had sound for a while; 48 kHz s16, `channels` interleaved */
    bool AudioOutWanted();
    void AudioOutFeed(const u8 *pcm, size_t n, u32 channels);

}
