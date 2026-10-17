#!/usr/bin/env python3
"""Verify real TCP packet boundaries, PCM tone and reconnect on loopback."""
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time

project = Path(__file__).resolve().parents[1]
fixture = project.parent / "logs/m83-runI/stream-first10.sft"
if not fixture.exists():
    raise SystemExit("Real Switch fixture missing: run from the full repository")
process = subprocess.Popen([sys.executable, str(project / "tools/replay_server.py"), str(fixture),
                            "--tone", "--bind", "127.0.0.1", "--no-beacon"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                           creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0)


def exact(client, n):
    result = bytearray()
    while len(result) < n:
        data = client.recv(min(n - len(result), 97))
        if not data:
            raise AssertionError("unexpected EOF")
        result.extend(data)
    return result


try:
    deadline = time.monotonic() + 5
    while True:
        try:
            client = socket.create_connection(("127.0.0.1", 9950), timeout=2)
            break
        except OSError:
            if time.monotonic() > deadline or process.poll() is not None:
                raise AssertionError("test server failed to start")
            time.sleep(.05)
    for session in range(2):
        with client:
            client.settimeout(5)
            for frame in range(3):
                audio = struct.unpack("<IHHIIIIII", exact(client,32))
                assert audio[:5] == (0x52544653,2,8,48000,2)
                assert 0 < audio[6] <= 65536 and audio[6] % 4 == 0
                pcm = exact(client,audio[6])
                samples = struct.unpack("<" + "h" * (len(pcm)//2), pcm)
                assert samples[::2] == samples[1::2] and max(samples) > 2000
                video = struct.unpack("<IHHIIIIII", exact(client,32))
                assert video[:5] == (0x52544653,2,6,1280,720)
                assert video[8] == frame
                assert exact(client,video[6]).startswith(b"\x00\x00\x00\x01\x67")
        if session == 0:
            time.sleep(.15)
            client = socket.create_connection(("127.0.0.1",9950),timeout=2)
    print("PASS: TCP fragmented reads, real H.264, stereo PCM tone, reconnect")
finally:
    process.terminate()
    output,_ = process.communicate(timeout=5)
    if process.returncode not in (0,1,-15):
        print(output)
