# tier4/mitm — the Atmosphère mitm frame tap

This directory's docs, in reading order:

- **[WRITEUP.md](WRITEUP.md)** — the technical story: why SysDVR is capped at
  720p30, what a plain sysmodule cannot reach, the `vi:u` mitm chain, and the
  libstratosphere patch that makes it possible. Start here.
- **[STATUS.md](STATUS.md)** — the research log. Opens with the current state,
  then every milestone in reverse chronological order with its evidence. This is
  the resume point for a fresh session, and the place dead ends are recorded so
  they are not retried.

The module itself is in **[../applet-mitm/](../applet-mitm/)**.

### Where it stands

Done, on hardware: live native 720p60 H.264 streaming over USB from handheld
mode, across game relaunches, for MK8D and BOTW (M83-M89). The pixels come
through the kernel's debug SVCs (the three graphics-stack routes are closed,
with evidence), the VIC converts them, NVENC encodes them, and
`tools/raw-recv/raw-view` shows them. **[PROJECT-HANDOFF.md](PROJECT-HANDOFF.md)**
is the map; the top-level README has the quick start.

### Historical

An early plan to reach frames via `vi` `GetIndirectLayerImageMap` was dropped
once, then revisited properly in M29–M31 (`CreateIndirectLayer` and its two
endpoint ABIs were reverse-engineered and do work) and closed for good: the
layer builds but stays empty, because only AM can attach an application's layer
to an indirect layer. See STATUS.md.
