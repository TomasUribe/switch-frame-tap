#!/usr/bin/env python3
"""Replay an SFTR recording over TCP/UDP to test the TV app without a Switch.

Only Python's standard library is required. This does not test USB or the real
Switch's network behaviour. Run on a PC in the same LAN as the TV.
"""
import argparse
import math
from pathlib import Path
import re
import socket
import struct
import threading
import time

HEADER = struct.Struct("<IHHIIIIII")
MAGIC = 0x52544653


def packets(path):
    with path.open("rb") as stream:
        while True:
            header = stream.read(32)
            if not header:
                return
            if len(header) != 32:
                raise ValueError("Truncated header")
            h = list(HEADER.unpack(header))
            if h[0] != MAGIC or h[1] != 2 or not 0 < h[6] <= 8 * 1024 * 1024:
                raise ValueError("Invalid SFTR v2 packet")
            data = stream.read(h[6])
            if len(data) != h[6]:
                raise ValueError("Truncated payload")
            if h[2] & 2 and any((nal[0] & 31) == 5 for nal in re.split(b"\x00\x00\x00?\x01", data) if nal):
                h[2] |= 4  # old IDR-only fixtures predate the explicit KEY bit
            yield h, data


def local_ip():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        try:
            probe.connect(("192.0.2.1", 9))  # selects a route; sends no packet
            return probe.getsockname()[0]
        except OSError:
            return "127.0.0.1"


def announce(stop, connected, ip):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        while not stop.wait(1):
            if not connected.is_set():
                try:
                    sock.sendto(f"SFTAP1 {ip} 9950 Switch Frame Tap test".encode(), ("255.255.255.255", 9951))
                except OSError:
                    pass  # manual IP remains available if broadcasts are filtered


def serve(args):
    # Validate the entire recording before accepting clients; do not load it into RAM.
    frames = sum(bool(h[2] & 2) for h, _ in packets(args.file))
    if not frames:
        raise ValueError("Recording contains no H.264 frames")
    stop, connected = threading.Event(), threading.Event()
    ip = local_ip()
    if not args.no_beacon:
        threading.Thread(target=announce, args=(stop, connected, ip), daemon=True).start()
    print(f"TV IP field: {ip} · TCP 9950 · {frames} video frames · {args.fps:g} fps", flush=True)
    print("Ctrl+C stops the server. Optional --tone generates 440 Hz PCM sound.", flush=True)
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind((args.bind, 9950))
            server.listen(1)
            while True:
                client, address = server.accept()
                connected.set()
                print(f"Connected: {address[0]}", flush=True)
                try:
                    with client:
                        client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                        client.settimeout(5)
                        sequence = audio_sequence = sample = 0
                        next_frame = time.monotonic()
                        while True:
                            for header, data in packets(args.file):
                                if header[2] & 8:
                                    if not args.tone:
                                        client.sendall(HEADER.pack(*header) + data)
                                    continue
                                if not header[2] & 2:
                                    continue
                                time.sleep(max(0, next_frame - time.monotonic()))
                                next_frame = max(next_frame + 1 / args.fps, time.monotonic())
                                if args.tone:
                                    samples = round(48000 / args.fps)
                                    pcm = bytearray()
                                    for n in range(samples):
                                        value = round(2500 * math.sin(2 * math.pi * 440 * (sample + n) / 48000))
                                        pcm.extend(struct.pack("<hh", value, value))
                                    sample += samples
                                    for offset in range(0, len(pcm), 65536):
                                        chunk = pcm[offset:offset + 65536]
                                        client.sendall(HEADER.pack(MAGIC, 2, 8, 48000, 2, 0, len(chunk), 0, audio_sequence) + chunk)
                                        audio_sequence = (audio_sequence + 1) & 0xffffffff
                                header[5] = 20000  # synthetic console age, not a measured latency
                                header[8] = sequence
                                client.sendall(HEADER.pack(*header) + data)
                                sequence = (sequence + 1) & 0xffffffff
                            # Loop boundaries are a new session: force decoder recovery at an IDR.
                            sequence = 0
                except (OSError, BrokenPipeError) as e:
                    print(f"Disconnected: {e}", flush=True)
                finally:
                    connected.clear()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path)
    parser.add_argument("--fps", type=float, default=60)
    parser.add_argument("--tone", action="store_true")
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--no-beacon", action="store_true", help="disable LAN announcements (local tests)")
    options = parser.parse_args()
    if not 1 <= options.fps <= 120:
        parser.error("--fps must be between 1 and 120")
    serve(options)
