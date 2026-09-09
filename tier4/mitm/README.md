# tier4/mitm — the Atmosphère mitm frame tap

This directory's docs, in reading order:

- **[WRITEUP.md](WRITEUP.md)** — the technical story: why SysDVR is capped, why a
  sysmodule can't beat it, the `vi:u` mitm chain, the libstratosphere patch, and
  the frame pipeline that's now reachable. Start here.
- **[STATUS.md](STATUS.md)** — current state and the concrete plan from here
  (driving the VIC and NVENC). This is the resume point for a fresh session.

The module itself is in **[../applet-mitm/](../applet-mitm/)**.

### Historical

An earlier plan (dropped) was to reach frames via `vi` `GetIndirectLayerImageMap`
— see git history around commits `56b4d4a`..`192f7be`. It required an
AppletResourceUserId + an `am` indirect-layer consumer handle, and
`GetIndirectLayerImageMap` turned out to be an applet-composition mechanism
(parent applet pulls a child library-applet's output), not a whole-screen tap.
The binder route in WRITEUP.md replaced it.
