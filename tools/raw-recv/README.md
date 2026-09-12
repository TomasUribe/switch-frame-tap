# raw-recv

Reads raw captured frames from `switch-frame-tap` over USB bulk and writes them
to disk. This is the **option A** transport: no codec, no protocol negotiation.

The sysmodule sends a 32-byte header then the raw Tegra block-linear slot; this
tool writes the payload to a `.bin` that `tools/deswizzle.py` turns into a PNG.

`switch-stream/receiver/` is a different thing — it speaks `sw_hdr_t` and pipes
through libavcodec, so it expects an **encoded** stream. That is option B and
needs NVENC.

## Build

```
cc -O2 -Wall -Wextra -o raw-recv raw-recv.c $(pkg-config --cflags --libs libusb-1.0)
```

## Permissions (one-time, avoids sudo)

```
sudo cp 99-switch-frame-tap.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

## Use

Start it before the probe fires — the sysmodule posts a chunk and waits for the
host to drain it. If nothing is listening it times out after 5 s and logs
`completion wait timed out`; the game is already resumed by then, so a mistimed
run costs a cycle and nothing worse.

```
until lsusb -d 1209:5f1e >/dev/null 2>&1; do sleep 1; done
./raw-recv -o /tmp/sft
python3 ../../tools/deswizzle.py /tmp/sft_000.bin /tmp/frame.png
```

Device is `1209:5f1e` (pid.codes open range), interface 0, bulk IN `0x81`.
The header layout must stay byte-identical to `SftHdr` in
`tier4/applet-mitm/source/applet_mitm_nv.cpp`.
