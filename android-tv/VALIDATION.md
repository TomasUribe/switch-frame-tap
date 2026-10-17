# Preview validation

Version 0.1.2 translates the interface and documentation into English. The playback
fixes introduced in 0.1.1 have passed host regressions. Confirmation on the reported
Vestel Cosmos / Android 11 device is still pending.

## Completed development checks

- `:app:assembleDebug`: builds a signed debug APK.
- `:app:protocolTest`: 40 checks from the full repository, including ten frames from
  the real capture at `logs/m83-runI/stream-first10.sft`.
- Little-endian headers, fragmented reads with intermediate timeouts, video/PCM
  separation, unsigned fields, and cancellation.
- Rejection of invalid headers, older protocol versions, oversized allocations,
  unaligned PCM, and unsupported formats before allocating payload storage.
- SPS/PPS extraction with three-byte and four-byte Annex-B start codes.
- Reference recovery at startup, sequence gaps, resolution changes, session
  restarts, and queue overflow/aging. Dependent frames are ignored until an IDR;
  unsigned sequence rollover is also checked.
- `:app:lintDebug`: no Android Lint errors. Warnings about target SDK, landscape
  orientation, and hardcoded UI strings remain preview limitations.
- Replay server: fragmented TCP reads, real H.264 packets, a stereo PCM tone,
  and reconnection. The automated test uses loopback without LAN announcements.
- APK: minimum API 23, Android TV launcher entry, optional USB feature, and signature
  verification with Android SDK tools.
- `:app:playbackTest`: production playback workers run against driver test doubles
  compiled separately from the APK. The regression scenarios cover:
  - First input buffer delayed by 60 ms: one decoder configuration, followed by output.
  - Output delayed by 180 ms with rewritten PTS: frames are presented rather than all discarded.
  - Hardware accepting input without output: recovery selects software decoding at an IDR.
  - Native audio requiring 160 ms to start: playback advances without repeated flushing.

Host checks **do not establish physical playback correctness or end-to-end latency**.
The development checks did not run the existing Switch sysmodule or PC viewers.
The user independently confirmed that Windows receives video and audio over the
network from the same console.

## Physical TV test plan

1. Install the APK and confirm it appears in the launcher. Navigate with the remote's
   arrow keys and check visible focus. Test **Close panel** before and after receiving
   video. It should remain closed until reopened with the remote.
2. Close the Windows viewer. Set Switch Frame Tap to Network and connect the TV using
   a manual IP address first. Check image, color, aspect ratio, and sound.
3. Enable **Show statistics** and record the TV model, Android version, resolution,
   received/decoded/presented FPS, decoder, queue/decode times, and resynchronizations.
   Try a 60 fps game and a 30 fps game.
4. Try 720p and 1080p, and High and Medium quality on the console. Check recovery
   after resolution changes. Compare Ethernet and Wi-Fi if available.
5. Return to the Switch HOME menu, close/reopen the game, disconnect/reconnect the
   network, and leave/return to the TV app. It should recover without reinstallation.
6. Set both ends to USB viewer. Connect the handheld Switch directly, allow USB
   access, and check video/audio. Unplug and reconnect the cable.
7. Deny USB access once: the app should not repeatedly display permission dialogs.
   Select **Connect** to request permission again. Test **Mute audio** and **Stable audio**.

To compare latency, use a slow-motion camera to record a counter or animation
visible simultaneously on the Switch and TV. The image difference approximates the
added delay; internal statistics are not a substitute. Keep quality and resolution
unchanged between recordings that are being compared.

## Troubleshooting

Report the failing step, displayed message, and a photo of the full statistics.
With ADB connected to the TV, `adb logcat -s SFT-TV` collects transport, audio, and
video errors. The replay server can isolate Android playback from the console streamer.

If connected without video or sound, select **Close panel** and inspect:

- **Video RX > 0, decoded = 0:** the decoder is not producing images.
- **Decoded > 0, shown > 0, but black:** investigate the Surface/driver output.
  Shown counts buffers submitted for rendering, not a physical panel measurement.
- **Audio RX = 0:** no audio packets are reaching the viewer.
- **Audio RX > 0, played = 0:** audio output is not advancing. Also check the muted
  state, audio focus, and actual TV buffer size.

Windows playback with the same Switch confirms that a usable stream exists.
These counters help locate a failure in the Android pipeline. The corrected failure
modes have been reproduced with driver test doubles; which one affected the reported
TV has not yet been confirmed.