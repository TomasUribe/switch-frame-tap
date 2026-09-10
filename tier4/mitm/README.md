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

### Where the frame problem stands

The output half is finished: a byte-exact VIC pipeline (block-linear →
linear, scale, format convert) driven from a sysmodule, with NVENC open. The
input half is the problem — three routes to another process's pixels are closed
with evidence, and the live one is the kernel's debug SVCs. STATUS.md's first
two sections cover both in full.

### Historical

An early plan to reach frames via `vi` `GetIndirectLayerImageMap` was dropped
once, then revisited properly in M29–M31 (`CreateIndirectLayer` and its two
endpoint ABIs were reverse-engineered and do work) and closed for good: the
layer builds but stays empty, because only AM can attach an application's layer
to an indirect layer. See STATUS.md.
