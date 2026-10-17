# Switch Frame Tap TV - preview 0.1.2

An **Android TV / Google TV viewer for Android 6.0 (API 23) and later**. It receives
H.264 video and PCM audio directly from Switch Frame Tap over a local network or
USB. The controller remains connected to the Switch; this app only receives and
plays video and audio. All application labels, status messages, and diagnostics
are in English.

This is a preview. The build, protocol parsing, and playback-worker regressions
have been checked on the development machine. An earlier preview connected on a
Vestel Cosmos running Android 11 but did not display video or play audio. Version
0.1.1 addressed playback issues reproduced with driver test doubles; confirmation
on that TV is still pending. Version 0.1.2 translates the interface and documentation
into English. **Successful playback and end-to-end latency on physical TVs remain
to be validated.**

## Installation and connection

1. Copy `SwitchFrameTapTV-0.1.2-preview.apk` to the TV or TV box and open it with a
   file manager that supports APK installation. Allow installation from that file
   manager when Android asks. The app appears as **Switch Frame Tap TV** in the TV
   launcher. This preview APK is signed with a development key.
2. Use the Switch Frame Tap manager on the console to choose the connection mode.
   Restart the console if requested. Keep the streamer installed and enabled.
3. **Network:** select Network on both the Switch and TV. Both devices must be on
   the same local network. Leave the IP address blank for UDP discovery and the
   last successful address, or enter the address displayed by the console's manager.
   Select **Connect**. Streaming uses TCP 9950; discovery uses UDP 9951. If the router
   filters announcements, enter the IP address manually. The app does not scan the subnet.
4. **USB:** select USB viewer on the Switch and USB on the TV. Connect the Switch's
   USB-C port directly to a USB host port on the TV with a data cable. Select
   **Connect** and allow USB access. Webcam/UVC modes are not supported by this
   receiver. The Switch must be handheld to leave its USB-C port available.
5. Open a game. The app hides the controls when it starts presenting video. Close
   the PC viewer first: the streamer supports only one connected viewer.

The Switch can remain docked when using Network. Changing the mode in the Android
app does not change the console configuration; select the matching mode on the
Switch as well.

## TV remote controls

- Use the arrow keys to navigate and OK to select a control.
- During playback, OK, Menu, or Back opens the controls. While connected, Back
  closes the controls and returns to the video.
- **Close panel**, at the top right, hides the controls without disconnecting.
  This also works without video. The panel stays closed until you open it with
  OK, Menu, or Back, allowing you to read the full statistics overlay.
- When disconnected and the panel is open, Back exits the app.
- **Mute audio** affects playback in the viewer, not the console.
- **Stable audio** requests a 60 ms buffer instead of 20 ms, subject to the output
  device's minimum. Leave it off to prioritize lower latency. Changing it restarts
  the connection and may help with audio dropouts.
- **Show statistics** displays received, decoded, and presented frame rates;
  bitrate; decoder selection; dropped frames; incoming audio; playback progress;
  the TV's actual audio buffer size; and audio focus state.

The app closes the connection and releases audio/USB resources when it goes into
the background. It reconnects on return if the connection was active.

## Quality and latency

The app preserves the received H.264 stream without re-encoding or reducing its
resolution. Quality is selected on the Switch, whose network sender can adjust it
to the available bandwidth. Start with High and 1080p if your configuration supports
them. If playback pauses or repeatedly resynchronizes, try Medium or 720p on the
console. Actual 1080p60 capture also depends on the game and Switch setup; the app
does not turn a 30 fps game into 60 fps.

Hardware H.264 decoders are preferred. On Android 11 and later, low-latency decoding
is requested when the driver advertises support. If an optimized configuration
fails, the app tries a basic configuration and other decoders, with software as a
last resort. Software decoding can increase latency.

Queues are bounded. The newest decoded output is presented, and input reference
recovery starts at an IDR after a gap or queue overflow. A device or connection
that cannot keep up can cause skipped frames or a pause until the next keyframe.
Maximum quality and a fixed low latency cannot be guaranteed on every TV.

Version 0.1.1 fixes repeated decoder initialization while waiting for the first
input buffer. It uses relative presentation timestamps and handles drivers that
rewrite them. It also removes the fixed 120 ms output rejection threshold, which
could discard every frame in a deeper pipeline. A decoder that accepts input but
produces no output is replaced at a subsequent IDR.

Audio is no longer flushed when video resynchronizes or whenever its native queue
exceeds 80 ms. Some TVs require a larger native buffer before playback can start.
AudioTrack bounds the system queue while the application keeps its own queue small.
On Android 12 and later, a low start threshold is requested when the driver allows it.

**Statistics do not measure total end-to-end latency.** Queue + decode measures
the interval from receiving the packet header to submitting decoded output for
display. It excludes network delay before arrival and the panel's own processing.
Console age comes from the sender and is a separate measurement. The protocol does
not provide a shared presentation clock for audio and video; the viewer limits
queue accumulation with small buffers.

## Compatibility

The same APK contains Java code and resources without CPU-specific native libraries.
It is intended for Android TV and Google TV devices, including Sony TVs running
Android TV, with **Android 6.0 or later**. There is no confirmed model compatibility
list yet. Sony TVs with another operating system or Android older than 6.0 are not
supported by this version.

USB is optional and detected at runtime. The TV firmware must expose USB host
support and permit access to device `1209:5f1e`, interface 0, bulk IN endpoint `0x81`.
A port that supports USB storage does not by itself confirm this capability.
Network reception does not depend on USB host support.

## Building

See [BUILDING.md](BUILDING.md) for requirements, environment setup, SDK installation,
build commands, tests, and optional distribution packaging.

With JDK 17 and Android SDK Platform 34 / Build Tools 34.0.0 configured, run from
`android-tv/` on Windows:

```powershell
.\gradlew.bat :app:assembleDebug :app:protocolTest :app:playbackTest :app:lintDebug
python tools/package.py
```

On Linux/macOS, use `bash gradlew` in place of `.\gradlew.bat`.
The APK is generated at `app/build/outputs/apk/debug/app-debug.apk`. Packaging copies
it to `../dist/` with the corresponding source archive and SHA-256 checksums.

This manually installed preview targets SDK 34. A Google Play release would require
reviewing the current target SDK and newer Android local-network permissions.

## Testing without a Switch

From the full repository root, on a PC on the same local network as the TV:

```shell
python android-tv/tools/replay_server.py logs/m83-runI/stream-first10.sft --tone
```

Enter the address printed by the server in the TV app and select **Connect**.
The server loops a real H.264 capture and generates a 440 Hz stereo PCM tone. This
allows testing Android decoding, audio, and TCP reconnection separately from the
console. The older capture contains only IDR frames; it does not replace testing
the current IDR/P stream, USB, or actual Switch latency. The fixture belongs to the
original repository and is not included in the Android source ZIP.

See [VALIDATION.md](VALIDATION.md) for completed checks and the pending TV test plan.

## Credits and license

Based on the protocol and viewer by **Tomas Uribe**,
[switch-frame-tap](https://github.com/TomasUribe/switch-frame-tap).
`LICENSE` contains GPL-2.0, and `UPSTREAM-NOTICE` preserves the upstream credits.
The distribution includes the corresponding source code for the Android viewer.

Official API references:
[MediaCodec](https://developer.android.com/reference/android/media/MediaCodec),
[low-latency decoding](https://source.android.com/docs/core/media/low-latency-media),
[AudioTrack](https://developer.android.com/reference/android/media/AudioTrack),
[USB](https://developer.android.com/reference/android/hardware/usb/UsbDeviceConnection),
[Android TV](https://developer.android.com/training/tv/get-started/create).