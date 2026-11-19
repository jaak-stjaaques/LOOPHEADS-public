# LOOPHEADS

LOOPHEADS is a four-tape instrument for the **M‑VAVE FM‑1**. Each tape loops at its
own length and plays through its own Karplus–Strong sound. Play a phrase, overdub
it, then let the loops drift in and out of alignment.

The idea is to make a small set of controls worth playing live. Change a scale
after recording, erase notes as the playhead passes them, or let **Memory Leak**
fade notes one by one. When a combination feels right, tap **SAVE** to keep it
as a single snapshot.

- **Four independent note loops** with overdubs, adjustable lengths, and per-tape volume, pan, and mute.
- **String, Tube, and Blow** resonators with selectable pings, an envelope, and an LFO on each tape.
- **Global scales and momentary transposition**, plus an arpeggiator and one-shot strums.
- **12-bit, 20 kHz Delay and Hall**, and quick snapshot recall during playback.

[Read the operator manual](docs/OPERATOR-MANUAL.md) · [Build and test](docs/BUILDING.md) · [Credits and licenses](LICENSING.md)

**Version 0.1.2** builds as `loopheads_0.1.2.fwsc`. The Mac tests and simulated
installer checks passed. Installation and power-cycle persistence on a physical
FM‑1 still need verification.

## Credits

LOOPHEADS builds on [Dinghy](https://github.com/zvenson/dinghy) by zvenson and the
[Felucca](https://github.com/hugelton/Felucca) and [SLOOP](https://github.com/isod89/sloop-fm1)
platform work it carries. Tape timing and loop boundaries draw on the earlier
LOOPHEAD tape engine, credited in the source to Kerem Kilic (Ellic Studio).
The synth implements the [Karplus–Strong method](https://doi.org/10.2307/3680062)
from Kevin Karplus and Alex Strong. The menus use
[IBM Plex Mono](https://github.com/IBM/plex).

The firmware is distributed under [GPL-3.0-only](LICENSE). The
[full credits](LICENSING.md) retain the upstream notices and list the separately
licensed assets.
