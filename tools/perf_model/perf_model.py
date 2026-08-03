#!/usr/bin/env python3
"""First-order goodput model for the FileFlow optical link.

STATUS: planning tool. Every input here is a HYPOTHESIS, not a measurement.
No number produced by this script may be reported as an achieved result.
See docs/specifications/PERFORMANCE-MODEL.md for the caveats, especially the
fact that these variables are NOT independent.

Usage:
    python3 tools/perf_model/perf_model.py            # markdown tables to stdout
    python3 tools/perf_model/perf_model.py --json     # machine-readable

Regenerate the tables in PERFORMANCE-MODEL.md with:
    python3 tools/perf_model/perf_model.py > /tmp/model.md
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass, asdict

# --- Milestones from the project charter, in bytes/second -------------------
MILESTONE_M4 = 200 * 1024  # "above 200 KB/s on reference phones"
MILESTONE_M5 = 500 * 1024  # "above 500 KB/s in a controlled stationary setup"
MILESTONE_M6 = 1024 * 1024  # "investigate whether 120 Hz devices can exceed 1 MB/s"


@dataclass
class Scenario:
    """One set of first-order link parameters.

    Attributes map to the variables named in the project charter:
      N          total logical cells per display state
      O          non-payload cell fraction (markers, pilots, header, guards)
      B          raw bits per payload cell (modulation order)
      r_fec      intra-frame FEC code rate
      fd         distinct display states per second
      pc         fraction of display states captured cleanly and decodable
      pm         fraction of MIXED states from which data is recovered
      h          header success probability
      r_fountain fountain efficiency (payload / delivered symbols)
    """

    name: str
    grid: tuple[int, int]
    O: float
    B: float
    r_fec: float
    fd: float
    pc: float
    pm: float
    h: float
    r_fountain: float
    note: str = ""

    @property
    def N(self) -> int:
        return self.grid[0] * self.grid[1]

    @property
    def payload_cells(self) -> float:
        return self.N * (1.0 - self.O)

    @property
    def raw_bits_per_state(self) -> float:
        """Every payload cell's raw modulated bits, before any coding."""
        return self.payload_cells * self.B

    @property
    def raw_bit_rate(self) -> float:
        """Raw optical bit rate: what the DISPLAY emits. Not goodput."""
        return self.raw_bits_per_state * self.fd

    @property
    def state_yield(self) -> float:
        """Fraction of emitted display states that yield usable symbols."""
        return min(1.0, self.pc + self.pm)

    @property
    def corrected_bit_rate(self) -> float:
        """After FEC overhead and lost/mixed states. Still not goodput."""
        return self.raw_bit_rate * self.r_fec * self.state_yield * self.h

    @property
    def goodput_bps(self) -> float:
        """Verified application-layer payload bits per second."""
        return self.corrected_bit_rate * self.r_fountain

    @property
    def goodput_bytes(self) -> float:
        return self.goodput_bps / 8.0

    @property
    def goodput_kbps(self) -> float:
        return self.goodput_bps / 1000.0

    def milestones(self) -> str:
        g = self.goodput_bytes
        hits = []
        if g >= MILESTONE_M6:
            hits.append("M6")
        if g >= MILESTONE_M5:
            hits.append("M5")
        if g >= MILESTONE_M4:
            hits.append("M4")
        return "+".join(reversed(hits)) if hits else "—"


def kb(x: float) -> str:
    return f"{x / 1024:.0f}"


GRIDS = {"96x160": (96, 160), "120x200": (120, 200), "144x240": (144, 240)}


def build_scenarios() -> list[Scenario]:
    """Conservative / expected / optimistic bands for each staged modulation.

    Rationale for each parameter choice is recorded in
    docs/specifications/PERFORMANCE-MODEL.md section "Parameter provenance".
    All are UNVALIDATED starting hypotheses.
    """
    s: list[Scenario] = []

    # --- M0 binary luminance, 30 display states/s (Phase 3 first light) -----
    s.append(Scenario("M0 binary @30, conservative", GRIDS["96x160"],
                      O=0.20, B=1, r_fec=0.70, fd=30, pc=0.55, pm=0.0,
                      h=0.95, r_fountain=0.90,
                      note="First end-to-end target. Small grid, heavy overhead."))
    s.append(Scenario("M0 binary @30, expected", GRIDS["120x200"],
                      O=0.15, B=1, r_fec=0.80, fd=30, pc=0.70, pm=0.0,
                      h=0.98, r_fountain=0.95,
                      note="Phase 3 exit criterion band."))

    # --- M0/M1 binary, 60 display states/s (Phase 4-5) ---------------------
    s.append(Scenario("M0 binary @60, conservative", GRIDS["96x160"],
                      O=0.18, B=1, r_fec=0.75, fd=60, pc=0.55, pm=0.0,
                      h=0.97, r_fountain=0.93))
    s.append(Scenario("M1 differential @60, expected", GRIDS["120x200"],
                      O=0.15, B=1, r_fec=0.80, fd=60, pc=0.70, pm=0.0,
                      h=0.98, r_fountain=0.95,
                      note="Differential costs nothing in cells but needs paired states."))
    s.append(Scenario("M1 differential @60, optimistic", GRIDS["144x240"],
                      O=0.12, B=1, r_fec=0.85, fd=60, pc=0.80, pm=0.0,
                      h=0.99, r_fountain=0.97,
                      note="Largest grid still resolvable; rigid mount."))

    # --- M2 four-level luminance (Phase 6) ---------------------------------
    s.append(Scenario("M2 four-level @60, conservative", GRIDS["96x160"],
                      O=0.20, B=2, r_fec=0.65, fd=60, pc=0.55, pm=0.0,
                      h=0.97, r_fountain=0.93,
                      note="Lower code rate pays for reduced symbol margin."))
    s.append(Scenario("M2 four-level @60, expected", GRIDS["120x200"],
                      O=0.15, B=2, r_fec=0.75, fd=60, pc=0.70, pm=0.0,
                      h=0.98, r_fountain=0.95))
    s.append(Scenario("M2 four-level @60, optimistic", GRIDS["144x240"],
                      O=0.12, B=2, r_fec=0.80, fd=60, pc=0.80, pm=0.0,
                      h=0.99, r_fountain=0.97))

    # --- M4 high-speed device profile, 120 states/s (Phase 8) --------------
    s.append(Scenario("M2 @120 states, expected", GRIDS["120x200"],
                      O=0.15, B=2, r_fec=0.75, fd=120, pc=0.55, pm=0.0,
                      h=0.97, r_fountain=0.95,
                      note="pc drops: more states/s means more mixed frames."))
    s.append(Scenario("M4 mixed-frame recovery @120", GRIDS["144x240"],
                      O=0.12, B=2, r_fec=0.80, fd=120, pc=0.55, pm=0.30,
                      h=0.98, r_fountain=0.97,
                      note="Research target. Requires rolling-shutter unmixing."))
    return s


def markdown(scenarios: list[Scenario]) -> str:
    out: list[str] = []
    out.append("| Scenario | Grid | N | O | B | Rfec | Fd | Pc | Pm | H | Rftn "
               "| Raw bit rate | Corrected | **Goodput** | Milestone |")
    out.append("|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|:--:|")
    for c in scenarios:
        out.append(
            f"| {c.name} | {c.grid[0]}×{c.grid[1]} | {c.N:,} | {c.O:.2f} | {c.B:g} "
            f"| {c.r_fec:.2f} | {c.fd:g} | {c.pc:.2f} | {c.pm:.2f} | {c.h:.2f} "
            f"| {c.r_fountain:.2f} | {c.raw_bit_rate / 1e6:.2f} Mb/s "
            f"| {c.corrected_bit_rate / 1e6:.2f} Mb/s "
            f"| **{kb(c.goodput_bytes)} KB/s** | {c.milestones()} |"
        )
    return "\n".join(out)


def sensitivity(base: Scenario) -> str:
    """One-at-a-time sensitivity. Honest caveat: the real variables covary."""
    # Only probability/rate-like variables are clamped to [0,1];
    # B (bits per cell) and fd (states per second) are unbounded above.
    unit_interval = {"O", "r_fec", "pc", "pm", "h", "r_fountain"}
    out = ["| Variable | −20% relative | baseline | +20% relative | "
           "Goodput swing |", "|---|--:|--:|--:|--:|"]
    for var in ("O", "B", "r_fec", "fd", "pc", "h", "r_fountain"):
        vals = []
        for mult in (0.8, 1.0, 1.2):
            kw = asdict(base)
            kw.pop("name"), kw.pop("grid"), kw.pop("note")
            scaled = getattr(base, var) * mult
            kw[var] = min(1.0, scaled) if var in unit_interval else scaled
            trial = Scenario(base.name, base.grid, note="", **kw)
            vals.append(trial.goodput_bytes)
        swing = (max(vals) - min(vals)) / base.goodput_bytes * 100
        out.append(f"| {var} | {kb(vals[0])} KB/s | {kb(vals[1])} KB/s "
                   f"| {kb(vals[2])} KB/s | {swing:.0f}% |")
    return "\n".join(out)


def grid_sweep() -> str:
    """Goodput vs grid size at fixed channel quality, binary and four-level."""
    out = ["| Grid | Cells | M0/M1 binary @60 | M2 four-level @60 |", "|---|--:|--:|--:|"]
    for name, g in GRIDS.items():
        row = []
        for b in (1, 2):
            c = Scenario("x", g, O=0.15, B=b, r_fec=0.80, fd=60, pc=0.70,
                         pm=0.0, h=0.98, r_fountain=0.95)
            row.append(f"{kb(c.goodput_bytes)} KB/s")
        out.append(f"| {name} | {g[0] * g[1]:,} | {row[0]} | {row[1]} |")
    return "\n".join(out)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    scenarios = build_scenarios()

    if args.json:
        print(json.dumps([{**asdict(s), "N": s.N,
                           "raw_bit_rate_bps": s.raw_bit_rate,
                           "corrected_bps": s.corrected_bit_rate,
                           "goodput_bytes_per_s": s.goodput_bytes}
                          for s in scenarios], indent=2))
        return

    print("### Scenario table\n")
    print(markdown(scenarios))
    print("\n### Grid-size sweep (fixed channel: O=0.15, Rfec=0.80, "
          "Fd=60, Pc=0.70, H=0.98, Rftn=0.95)\n")
    print(grid_sweep())
    base = next(s for s in scenarios if s.name == "M1 differential @60, expected")
    print(f"\n### One-at-a-time sensitivity around `{base.name}`\n")
    print(sensitivity(base))
    print("\n_Caveat: the one-at-a-time sweep is misleading where variables "
          "covary — raising B or Fd is expected to REDUCE Pc and Rfec. "
          "See PERFORMANCE-MODEL.md._")


if __name__ == "__main__":
    main()
