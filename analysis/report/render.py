"""
The session report as one self-contained HTML page: inline CSS and SVG, no
scripts, no external files, readable in light and dark themes, and printable
(the browser's "Save as PDF" gives a clean document).
"""

from __future__ import annotations

import base64
import html
import math
import time
from pathlib import Path

from .collect import PER_CAMERA_ANALYSES, SESSION_ANALYSES, CameraResults, SessionData, duration_s

CSS = """
:root {
  --bg: #f7f7f9; --card: #ffffff; --ink: #1d1d27; --muted: #63637a; --line: #e2e2ea;
  --accent: #3d5afe; --a: #2e9d5b; --b: #2f80d1; --c: #d08a1a; --d: #c2410c; --e: #8b5cf6;
  --grid: #ececf2;
  --l-subj: #2e9d5b; --l-other: #2f80d1; --l-eye: #0f8a80; --l-blink: #5b6478;
  --l-smile: #d9951f; --l-duch: #9a5a06; --l-brow: #8b5cf6; --l-nod: #0e7fa8; --l-shake: #c2410c;
  --q1: #2e9d5b; --q2: #7cb342; --q3: #d9951f; --q4: #c2410c;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --bg: #12121a; --card: #1b1b26; --ink: #e8e8f0; --muted: #9a9ab0; --line: #2c2c3c;
    --accent: #8c9eff; --a: #4cc584; --b: #5aa6f0; --c: #f0b44c; --d: #f08a5d; --e: #b39bff;
    --grid: #262634;
    --l-subj: #4cc584; --l-other: #5aa6f0; --l-eye: #3cc6b8; --l-blink: #a3acc0;
    --l-smile: #f0b44c; --l-duch: #ffd27f; --l-brow: #b39bff; --l-nod: #49b8e0; --l-shake: #f08a5d;
    --q1: #4cc584; --q2: #a5d65c; --q3: #f0b44c; --q4: #f08a5d;
  }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--ink);
  font: 15px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
main { max-width: 1080px; margin: 0 auto; padding: 24px 16px 48px; }
h1 { font-size: 26px; margin: 0 0 4px; }
h2 { font-size: 19px; margin: 32px 0 10px; }
h3 { font-size: 15px; margin: 18px 0 6px; color: var(--muted); text-transform: uppercase;
  letter-spacing: .04em; }
.sub { color: var(--muted); margin: 0 0 12px; }
.chips span { display: inline-block; border: 1px solid var(--line); border-radius: 999px;
  padding: 2px 10px; margin: 0 6px 6px 0; font-size: 13px; color: var(--muted); }
.card { background: var(--card); border: 1px solid var(--line); border-radius: 12px;
  padding: 16px 18px; margin: 12px 0; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(330px, 1fr)); gap: 12px; }
.grid .card { margin: 0; }
.grid h3 { margin-top: 0; }
.tiles { display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 10px; }
.tile { background: var(--card); border: 1px solid var(--line); border-radius: 10px;
  padding: 10px 12px; }
.tile .v { font-size: 22px; font-weight: 650; }
.tile .k { font-size: 12px; color: var(--muted); }
table { border-collapse: collapse; width: 100%; font-size: 14px; }
td, th { text-align: left; padding: 5px 8px; border-bottom: 1px solid var(--line);
  vertical-align: top; }
th { color: var(--muted); font-weight: 600; }
th.n { text-align: right; }
td.n { text-align: right; font-variant-numeric: tabular-nums; }
.note { color: var(--muted); font-size: 13px; }
.missing li { color: var(--muted); }
svg { width: 100%; height: auto; display: block; }
svg text { fill: var(--muted); font: 11px system-ui, sans-serif; }
.notes { white-space: pre-wrap; }
.dot { display: inline-block; width: 10px; height: 10px; border-radius: 50%; margin-right: 6px; }
.dot.ok { background: var(--q1); } .dot.warn { background: var(--q3); }
.dot.bad { background: var(--q4); } .dot.info { background: var(--line); }
.scroll { max-height: 320px; overflow: auto; }
.scroll td:first-child { white-space: nowrap; color: var(--muted); }
.scroll td:nth-child(2) { white-space: nowrap; }
.scroll td:last-child { width: 100%; }
.stills { display: grid; grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 10px; }
.stills figure { margin: 0; } .stills img { width: 100%; border-radius: 8px; display: block; }
.stills figcaption { font-size: 12px; color: var(--muted); }
nav.toc { position: sticky; top: 0; z-index: 1; background: var(--bg); padding: 8px 0;
  border-bottom: 1px solid var(--line); font-size: 13px; }
nav.toc a { color: var(--accent); text-decoration: none; margin-right: 14px; white-space: nowrap; }
h2 { scroll-margin-top: 48px; }
@media print {
  @page { margin: 14mm; }
  body { background: #fff; }
  * { -webkit-print-color-adjust: exact; print-color-adjust: exact; }
  nav.toc { display: none; }
  .scroll { max-height: none; overflow: visible; }
  .card, .tile, figure { break-inside: avoid; }
  h2, h3 { break-after: avoid; }
}
"""

LANE_H, LANE_GAP, LABEL_W, CHART_W = 16, 6, 132, 1000
APPROX_AUDIO = "; audio placed approximately (no audio timing file)"


def esc(x) -> str:
    return html.escape("" if x is None else str(x))


def num(v, digits: int = 0, unit: str = "") -> str:
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "–"
    s = f"{float(v):.{digits}f}" if isinstance(v, int | float) else esc(v)
    return f"{s}{unit}"


def clock(seconds: float) -> str:
    seconds = max(0, int(round(seconds)))
    return f"{seconds // 60}:{seconds % 60:02d}"


# ── Timeline ─────────────────────────────────────────────────────────────────


def _nice_step(span_s: float) -> float:
    for step in (1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600):
        if span_s / step <= 10:
            return float(step)
    return 7200.0


def rppg_tier(snr_db, valid_fraction) -> int:
    """Signal quality of one heart-rate window, 1 (best) to 4, as the app's
    badge (``rppg_quality_for`` in ``src/analysis/realtime_metrics.cpp``)."""
    if valid_fraction is not None and valid_fraction < 0.6:
        return 4
    if snr_db is None:
        return 3
    return 1 if snr_db >= 5.0 else 2 if snr_db >= 0.0 else 3 if snr_db >= -5.0 else 4


RPPG_TIERS = ((1, "clear"), (2, "good"), (3, "weak"), (4, "poor"))


def timeline_svg(cam: CameraResults) -> str | None:
    """Time-aligned lanes of everything measured on one camera; one colour
    per lane, so it reads without a legend."""
    r = cam.results
    if cam.t0_ms is None:  # no timestamps file: times cannot be placed on the video
        return None
    # (label, [(t0_s, t1_s or None for a tick, colour var, opacity)])
    lanes: list[tuple[str, list]] = []
    t0 = cam.t0_ms

    def rel(ms):
        return (ms - t0) / 1000.0

    conv = r.get("conversation")
    if conv:
        for who, label, colour in (
            ("subject", "On camera speaks", "--l-subj"),
            ("other", "Other speaks", "--l-other"),
        ):
            spans = [
                (rel(s["start_ms"]), rel(s["end_ms"]), colour, 1.0)
                for s in conv["spurts"]
                if s["speaker"] == who
            ]
            lanes.append((label, spans))
    ec = r.get("eye_contact")
    if ec:
        # Solid: looking at the partner; faint: looking away; blank: no face.
        spans = [(rel(a), rel(b), "--l-eye", 1.0 if c else 0.28) for a, b, c in ec["runs"]]
        lanes.append(("Eye contact", spans))
    fd = r.get("face_dynamics")
    if fd:
        ev = fd["events"]
        duch = {e["start_ms"] for e in ev if e["kind"] == "duchenne_smile"}

        def events(kind, colour):
            return [
                (rel(e["start_ms"]), rel(e["end_ms"]), colour, 1.0) for e in ev if e["kind"] == kind
            ]

        lanes += [
            (
                "Blinks",
                [(rel(e["peak_ms"]), None, "--l-blink", 1.0) for e in ev if e["kind"] == "blink"],
            ),
            (
                "Smiles",
                [
                    (
                        rel(e["start_ms"]),
                        rel(e["end_ms"]),
                        "--l-duch" if e["start_ms"] in duch else "--l-smile",
                        1.0,
                    )
                    for e in ev
                    if e["kind"] == "smile"
                ],
            ),
            (
                "Brow raises",
                [
                    (rel(e["peak_ms"]), None, "--l-brow", 1.0)
                    for e in ev
                    if e["kind"] == "brow_raise"
                ],
            ),
            ("Nods", events("nod", "--l-nod")),
            ("Head shakes", events("shake", "--l-shake")),
        ]
    # Heart rate: each window's rate over its own stretch, coloured by quality.
    hr_steps = []  # (t0_s, t1_s, bpm, tier)
    hr = r.get("rppg")
    if hr:
        wins = sorted(hr.get("windows", []), key=lambda w: w["start_ms"])
        centres = [(w["start_ms"] + w["end_ms"]) / 2.0 for w in wins]
        for i, w in enumerate(wins):
            bpm = w.get("smoothed_bpm") or w.get("bpm")
            if bpm is None:
                continue
            a = (centres[i] + centres[i - 1]) / 2.0 if i else w["start_ms"]
            b = (centres[i] + centres[i + 1]) / 2.0 if i + 1 < len(wins) else w["end_ms"]
            tier = rppg_tier(w.get("snr_db"), w.get("valid_frame_fraction"))
            hr_steps.append((rel(a), rel(b), bpm, tier))
    lanes = [(label, items) for label, items in lanes if items]
    if not lanes and not hr_steps:
        return None
    times = [t for _, items in lanes for a, b, _c, _o in items for t in (a, b) if t is not None]
    times += [t for a, b, _v, _q in hr_steps for t in (a, b)]
    # The whole video, so every camera's lanes line up with its own length.
    t_max = max(times + [cam.duration_s or 0.0, 1.0])
    plot_w = CHART_W - LABEL_W - 10
    x = lambda t: LABEL_W + max(0.0, min(t, t_max)) / t_max * plot_w  # noqa: E731
    hr_h = 90 if hr_steps else 0
    height = len(lanes) * (LANE_H + LANE_GAP) + hr_h + 40
    out = [
        f'<svg viewBox="0 0 {CHART_W} {height}" role="img" aria-label="Timeline of '
        f'{esc(cam.name)}">'
    ]
    step = _nice_step(t_max)
    k = 0.0
    while k <= t_max + 1e-9:
        xx = x(k)
        out.append(
            f'<line x1="{xx:.1f}" y1="0" x2="{xx:.1f}" y2="{height - 18}" stroke="var(--grid)"/>'
        )
        out.append(f'<text x="{xx:.1f}" y="{height - 4}" text-anchor="middle">{clock(k)}</text>')
        k += step
    y = 4
    for label, items in lanes:
        out.append(
            f'<text x="{LABEL_W - 8}" y="{y + LANE_H - 4}" text-anchor="end">{esc(label)}</text>'
        )
        out.append(
            f'<rect x="{LABEL_W}" y="{y}" width="{plot_w}" height="{LANE_H}" fill="var(--grid)" '
            'opacity=".35" rx="3"/>'
        )
        for a, b, colour, op in items:
            if b is None:
                xx = x(a)
                out.append(
                    f'<line x1="{xx:.1f}" y1="{y + 2}" x2="{xx:.1f}" y2="{y + LANE_H - 2}" '
                    f'stroke="var({colour})" stroke-width="1.6"/>'
                )
            else:
                w = max(1.0, x(b) - x(a))
                out.append(
                    f'<rect x="{x(a):.1f}" y="{y + 2}" width="{w:.1f}" height="{LANE_H - 4}" '
                    f'fill="var({colour})" opacity="{op:g}" rx="2"/>'
                )
        y += LANE_H + LANE_GAP
    if hr_steps:
        vals = [v for _a, _b, v, _q in hr_steps]
        lo, hi = math.floor(min(vals)) - 2, math.ceil(max(vals)) + 2
        if hi - lo < 10:
            mid = (hi + lo) / 2.0
            lo, hi = math.floor(mid - 5), math.ceil(mid + 5)
        top, bottom = y + 8, y + hr_h - 8
        yy = lambda v: bottom - (v - lo) / (hi - lo) * (bottom - top)  # noqa: E731
        out.append(f'<text x="{LABEL_W - 8}" y="{top + 30}" text-anchor="end">Heart rate</text>')
        out.append(f'<text x="{LABEL_W - 8}" y="{top + 44}" text-anchor="end">(bpm)</text>')
        for v in (lo, (lo + hi) / 2.0, hi):
            out.append(
                f'<line x1="{LABEL_W}" y1="{yy(v):.1f}" x2="{LABEL_W + plot_w}" y2="{yy(v):.1f}" '
                'stroke="var(--grid)"/>'
            )
            out.append(f'<text x="{LABEL_W + 4}" y="{yy(v) - 2:.1f}" font-size="10">{v:.0f}</text>')
        for a, b, v, q in hr_steps:
            out.append(
                f'<line x1="{x(a):.1f}" y1="{yy(v):.1f}" x2="{x(b):.1f}" y2="{yy(v):.1f}" '
                f'stroke="var(--q{q})" stroke-width="3" stroke-linecap="round"/>'
            )
    out.append("</svg>")
    return "".join(out)


def swatch(colour: str, text: str, opacity: float = 1.0) -> str:
    return (
        '<span style="display:inline-flex;align-items:center;margin-right:14px">'
        f'<span style="width:12px;height:12px;border-radius:3px;background:var({colour});'
        f'opacity:{opacity:g};margin-right:6px"></span>{esc(text)}</span>'
    )


def legend(cam: CameraResults) -> str:
    """Only what the lane labels do not say: shades within a lane."""
    r = cam.results
    parts = []
    if "eye_contact" in r:
        parts += [
            swatch("--l-eye", "eye contact"),
            swatch("--l-eye", "looking away", 0.28),
            "<span style='margin-right:14px'>blank: no face</span>",
        ]
    if "face_dynamics" in r:
        parts += [swatch("--l-smile", "smile"), swatch("--l-duch", "Duchenne smile")]
    if "rppg" in r:
        parts += [swatch(f"--q{q}", f"heart rate, {name} signal") for q, name in RPPG_TIERS]
    return f'<p class="note">{"".join(parts)}</p>' if parts else ""


# ── Small charts ─────────────────────────────────────────────────────────────


def bars_svg(
    categories: list, series: list, unit: str = "", w: int = 340, h: int = 170, top_value=None
) -> str:
    """Stacked vertical bars: ``series`` is [(name, colour var, values per category)];
    the axis runs to ``top_value``, else to the tallest bar."""
    left, bottom, top = 34, 34, 10
    plot_w, plot_h = w - left - 8, h - bottom - top
    totals = [sum(v[i] for _n, _c, v in series) for i in range(len(categories))]
    peak = top_value or max(totals + [1])
    bw = plot_w / max(1, len(categories))
    out = [f'<svg viewBox="0 0 {w} {h}" role="img">']
    for frac in (0.0, 0.5, 1.0):
        yy = top + plot_h * (1 - frac)
        out.append(
            f'<line x1="{left}" y1="{yy:.1f}" x2="{w - 8}" y2="{yy:.1f}" stroke="var(--grid)"/>'
        )
        label = f"{peak * frac:.0f}{unit}"
        out.append(f'<text x="{left - 4}" y="{yy + 4:.1f}" text-anchor="end">{label}</text>')
    for i, cat in enumerate(categories):
        base = top + plot_h
        for _name, colour, values in series:
            v = values[i]
            if v <= 0:
                continue
            bh = v / peak * plot_h
            base -= bh
            out.append(
                f'<rect x="{left + i * bw + 2:.1f}" y="{base:.1f}" width="{bw - 4:.1f}" '
                f'height="{bh:.1f}" fill="var({colour})" rx="2"/>'
            )
        if len(categories) <= 8 or i % 2 == 0:
            out.append(
                f'<text x="{left + (i + 0.5) * bw:.1f}" y="{h - bottom + 14}" '
                f'text-anchor="middle">{esc(cat)}</text>'
            )
    out.append("</svg>")
    return "".join(out)


FTO_EDGES = list(range(-1000, 2001, 250))  # ms; the outer bins also take what is beyond


def _response_chart(conv: dict) -> str | None:
    tr = [t for t in conv.get("transitions", []) if t.get("fto_ms") is not None]
    if not tr:
        return None
    n = len(FTO_EDGES) - 1
    counts = {"subject": [0] * n, "other": [0] * n}
    for t in tr:
        k = sum(1 for e in FTO_EDGES[1:-1] if t["fto_ms"] >= e)
        counts["subject" if t.get("to") == "subject" else "other"][k] += 1
    cats = [f"{FTO_EDGES[i] / 1000:g}" for i in range(n)]
    svg = bars_svg(
        cats,
        [
            ("on camera answers", "--l-subj", counts["subject"]),
            ("other answers", "--l-other", counts["other"]),
        ],
    )
    return (
        "<h3>Response times</h3>"
        + svg
        + "<p class='note'>"
        + swatch("--l-subj", "on camera answers")
        + swatch("--l-other", "other answers")
        + "<br>Seconds from the end of one speaker to the start of the next (bins of 0.25 s, "
        "from the label); below 0: they overlapped. The outer bins hold everything beyond.</p>"
    )


def _eye_contact_by_state(ec: dict) -> str | None:
    sm = ec["summary"]
    states = [
        ("speaking", sm.get("eye_contact_speaking_pct")),
        ("listening", sm.get("eye_contact_listening_pct")),
        ("silence", sm.get("eye_contact_silence_pct")),
    ]
    if all(v is None for _k, v in states):
        return None
    cats = [k for k, _v in states]
    vals = [v or 0.0 for _k, v in states]
    return (
        "<h3>Eye contact while...</h3>"
        + bars_svg(cats, [("eye contact", "--l-eye", vals)], "%", h=150, top_value=100.0)
        + "<p class='note'>Share of each state's time spent in eye contact.</p>"
    )


# ── Data quality ─────────────────────────────────────────────────────────────


def _status(ok: bool, warn: bool) -> str:
    return "ok" if ok else "warn" if warn else "bad"


def quality_rows(data: SessionData) -> list:
    """(status, what, detail): whether the recording and each analysis can be
    trusted, before anyone reads the numbers."""
    rows = []
    meta = data.meta
    end = meta.get("session_end")
    if "session_end" in meta:
        clean = isinstance(end, dict) and end.get("ended_cleanly", True)
        rows.append(
            (
                "ok" if clean else "bad",
                "Recording",
                "ended cleanly" if clean else "did not end cleanly (interrupted or still running)",
            )
        )
    sy = data.session_results.get("sync_repair")
    by_cam = {c.get("index"): c for c in (sy or {}).get("cameras", [])}
    for cam in data.cameras:
        c = by_cam.get(cam.index)
        if c is None:
            rows.append(("info", f"{cam.name} frames", "Frame Sync not run"))
        elif c.get("skipped"):
            rows.append(("bad", f"{cam.name} frames", f"skipped: {c.get('skip_reason') or ''}"))
        else:
            total = c.get("output_frame_count") or 0
            missing = c.get("missing_frame_count") or 0
            pct = 100.0 * missing / total if total else 0.0
            extra = "; alignment uncertain" if c.get("alignment_uncertain") else ""
            rows.append(
                (
                    _status(pct < 0.5 and not extra, pct < 2.0),
                    f"{cam.name} frames",
                    f"{missing} of {total} missing ({pct:.1f}%) in {c.get('gap_count') or 0} "
                    f"gap(s), filled with the previous frame{extra}",
                )
            )
        r = cam.results
        seen = ((r.get("face_dynamics") or {}).get("summary") or {}).get("face_seen_pct")
        if seen is None:
            seen = ((r.get("eye_contact") or {}).get("summary") or {}).get("face_seen_pct")
        if seen is not None:
            rows.append(
                (
                    _status(seen >= 90, seen >= 60),
                    f"{cam.name} face seen",
                    f"{seen:.0f}% of the video (face analyses only measure there)",
                )
            )
        wins = (r.get("rppg") or {}).get("windows") or []
        if wins:
            tiers = [rppg_tier(w.get("snr_db"), w.get("valid_frame_fraction")) for w in wins]
            good = 100.0 * sum(1 for t in tiers if t <= 2) / len(tiers)
            rows.append(
                (
                    _status(good >= 70, good >= 40),
                    f"{cam.name} heart-rate signal",
                    f"{good:.0f}% of windows clear or good",
                )
            )
        cal = next(
            (c.get("calibration") for c in meta.get("cameras", []) if c.get("index") == cam.index),
            None,
        )
        if cal is not None:
            if cal.get("calibrated"):
                rms = cal.get("rms_error")
                rms = -1.0 if rms is None else float(rms)
                model = cal.get("distortion_model") or ""
                held = cal.get("holdout_rms_error", -1)
                detail = (f"lens {rms:.2f} px" if rms >= 0 else "lens calibrated") + (
                    f", {model}" if model else ""
                )
                if held is not None and held >= 0:
                    detail += f" (held-out {held:.2f} px)"
                detail += "; room " + ("calibrated" if cal.get("extrinsic_calibrated") else "not")
                rows.append(
                    (_status(0 <= rms < 1.0, 0 <= rms < 2.0), f"{cam.name} calibration", detail)
                )
            else:
                rows.append(("info", f"{cam.name} calibration", "no lens calibration"))
    for t in data.session_results.get("transcripts") or []:
        st = t.get("diarization")
        rows.append(
            (
                "ok" if st == "ok" else "warn",
                f"Speakers in {t['audio']}",
                "told apart (diarization)" if st == "ok" else f"diarization: {st or 'not run'}",
            )
        )
    q = ((data.session_results.get("pose3d") or {}).get("quality")) or {}
    ident = q.get("identity") or {}
    if ident:
        bad = ident.get("swaps", 0) + ident.get("jumps", 0)
        rows.append(
            (
                _status(bad == 0, bad <= 3),
                "3D pose identities",
                f"{ident.get('track_count')} track(s), {ident.get('jumps')} jump(s), "
                f"{ident.get('swaps')} likely swap(s), {ident.get('short_tracks')} under 1 s",
            )
        )
    tl = q.get("timeline") or {}
    if tl.get("complete_pct") is not None:
        missing = ", ".join(
            f"{CAUSE_NAMES.get(c, c)} {s:g} s"
            for c, s in tl.get("seconds_by_cause", {}).items()
            if s
        )
        pct = tl["complete_pct"]
        nobody = tl.get("nobody_s") or 0
        rows.append(
            (
                _status(pct >= 90, pct >= 60),
                "3D pose coverage",
                f"everyone seen in 3D in {pct:.0f}% of the seconds with someone in view"
                + (f"; people missing: {missing}" if missing else "")
                + (f"; nobody in view {nobody:g} s" if nobody else ""),
            )
        )
    elif tl.get("windows"):
        rows.append(("bad", "3D pose coverage", "no camera saw anyone: check the Pose results"))
    for cam, v in (q.get("views") or {}).items():
        pct = v.get("dropped_pct")
        if pct is not None:
            rows.append(
                (
                    _status(pct < 5, pct < 15),
                    f"Camera {int(cam) + 1} in 3D pose",
                    f"dropped as disagreeing from {pct:.1f}% of the keypoints it saw, "
                    f"{v.get('unresolved', 0)} unresolved",
                )
            )
    return rows


def quality_section(data: SessionData) -> str:
    rows = quality_rows(data)
    if not rows:
        return ""
    body = "".join(
        f"<tr><td><span class='dot {st}'></span>{esc(what)}</td><td>{esc(detail)}</td></tr>"
        for st, what, detail in rows
    )
    return (
        "<h2 id='quality'>Data quality</h2><div class='card'><table>"
        f"{body}</table><p class='note'>Green: fine; amber: read the numbers with care; red: "
        "a problem worth fixing first; grey: not known.</p></div>"
    )


def _png_data_uri(path: Path | None) -> str | None:
    try:
        return "data:image/png;base64," + base64.b64encode(Path(path).read_bytes()).decode("ascii")
    except (OSError, TypeError):
        return None


def voice_svg(v: dict) -> str | None:
    """The spectrogram run_voice.py drew, with a time axis, its frequency
    scale, and the pitch track on its own scale, as the app shows it."""
    uri = _png_data_uri(v.get("image"))
    sp, pitch = v.get("spectrogram") or {}, v.get("pitch") or {}
    if uri is None or not sp:
        return None
    t0, t1 = (sp.get("t0_ms") or 0.0) / 1000.0, (sp.get("t1_ms") or 0.0) / 1000.0
    f1 = sp.get("f1_hz") or 0.0
    if t1 <= t0 or f1 <= 0:
        return None
    w, h, left, right, top, bottom = CHART_W, 230, 46, 46, 6, 22
    pw, ph = w - left - right, h - top - bottom
    x = lambda t: left + (t - t0) / (t1 - t0) * pw  # noqa: E731
    out = [
        f'<svg viewBox="0 0 {w} {h}" role="img" aria-label="Spectrogram of {esc(v["audio"])}">',
        f'<image href="{uri}" x="{left}" y="{top}" width="{pw}" height="{ph}" '
        'preserveAspectRatio="none"/>',
    ]
    for frac in (0.0, 0.5, 1.0):
        yy = top + ph * (1 - frac)
        out.append(
            f'<text x="{left - 4}" y="{yy + 4:.1f}" text-anchor="end">'
            f"{f1 * frac / 1000:.1f}k</text>"
        )
    step = _nice_step(t1 - t0)
    k = math.ceil(t0 / step) * step
    while k <= t1 + 1e-9:
        out.append(f'<text x="{x(k):.1f}" y="{h - 6}" text-anchor="middle">{clock(k)}</text>')
        k += step
    vals = pitch.get("values_hz") or []
    lo, hi = pitch.get("floor_hz"), pitch.get("ceiling_hz")
    if vals and lo and hi and hi > lo:
        pt0 = (pitch.get("t0_ms") or 0.0) / 1000.0
        dt = (pitch.get("dt_ms") or 10.0) / 1000.0
        yy = lambda f: top + ph * (1 - (f - lo) / (hi - lo))  # noqa: E731
        run: list = []
        runs = []
        for i, f in enumerate(vals):
            if f and f > 0 and t0 <= pt0 + i * dt <= t1:  # only over the picture
                run.append(f"{x(pt0 + i * dt):.1f},{yy(min(max(f, lo), hi)):.1f}")
            elif run:
                runs.append(run)
                run = []
        runs.append(run)
        for r_ in runs:
            if len(r_) > 1:
                out.append(
                    f'<polyline points="{" ".join(r_)}" fill="none" stroke="var(--l-smile)" '
                    'stroke-width="2"/>'
                )
        for f in (lo, hi):
            out.append(
                f'<text x="{w - right + 4}" y="{yy(f) + 4:.1f}" fill="var(--l-smile)">'
                f"{f:.0f} Hz</text>"
            )
    out.append("</svg>")
    return "".join(out)


#: Why people were missing from 3D (pose3d/audit.py), in words, with a colour.
CAUSE_NAMES = {
    "no_frames": "no camera frames",
    "single_view_only": "only one camera saw anyone",
    "one_view_or_unmatched": "someone seen by one camera only, or not matched",
    "not_triangulated": "cameras did not agree",
    "nobody": "nobody in view",
}
CAUSE_COLOURS = {
    "no_frames": "--q4",
    "single_view_only": "--q3",
    "one_view_or_unmatched": "--l-brow",
    "not_triangulated": "--l-other",
    "nobody": "--line",
}


def coverage_svg(tl: dict) -> str | None:
    """The 3D pose timeline as one strip: grey where everyone was in 3D, a
    colour per cause where someone was missing."""
    wins = tl.get("windows") or []
    if not wins:
        return None
    step = tl.get("window_s", 1.0)
    end = wins[-1]["t0_s"] + step
    w, h, left = CHART_W, 46, 8
    x = lambda t: left + t / end * (w - 2 * left)  # noqa: E731
    out = [f'<svg viewBox="0 0 {w} {h}" role="img" aria-label="3D pose coverage">']
    for win in wins:
        colour = CAUSE_COLOURS.get(win["cause"], "--grid")
        out.append(
            f'<rect x="{x(win["t0_s"]):.1f}" y="4" width="{max(1.0, x(step) - x(0)):.1f}" '
            f'height="18" fill="var({colour})"/>'
        )
    k = 0.0
    tick = _nice_step(end)
    while k <= end + 1e-9:
        out.append(f'<text x="{x(k):.1f}" y="38" text-anchor="middle">{clock(k)}</text>')
        k += tick
    out.append("</svg>")
    used = {win["cause"] for win in wins if win["cause"]}
    legend_html = "".join(
        swatch(CAUSE_COLOURS[c], CAUSE_NAMES[c]) for c in CAUSE_NAMES if c in used
    )
    return (
        "".join(out) + f"<p class='note'>{swatch('--grid', 'everyone seen in 3D')}{legend_html}</p>"
    )


SOCIAL_NAMES = {"head": "Head movement", "body": "Body movement", "heart_rate": "Heart rate"}


def social_html(so: dict) -> str:
    """Synchrony and mimicry between the two people (run_social.py)."""
    cams = so.get("cameras") or {}
    a = f"Camera {cams['a'] + 1}" if isinstance(cams.get("a"), int) else "A"
    b = f"Camera {cams['b'] + 1}" if isinstance(cams.get("b"), int) else "B"
    rows = []
    for key, s in (so.get("synchrony") or {}).items():
        lead = ""
        if s.get("a_leads_pct") is not None:
            lead = (
                f"; A leads {num(s['a_leads_pct'], 0, '%')}, B leads "
                f"{num(s.get('b_leads_pct'), 0, '%')} (median lag "
                f"{num(s.get('median_lag_s'), 1, ' s')})"
            )
        rows.append(
            (
                f"{SOCIAL_NAMES.get(key, key)} synchrony",
                f"peak r {num(s.get('mean_peak_r'), 2)} vs "
                f"{num(s.get('surrogate_mean_peak_r'), 2)} by chance "
                f"(p = {num(s.get('p_value'), 2)}){esc(lead)}",
            )
        )
    for kind, d in (so.get("mimicry") or {}).items():
        for direction, who in (("a_to_b", "B after A"), ("b_to_a", "A after B")):
            m = d.get(direction) or {}
            if m.get("rate") is None:
                continue
            rows.append(
                (
                    f"{kind.capitalize()} mimicry, {who}",
                    f"{num(100 * m['rate'], 0, '%')} of {num(m.get('events'))} "
                    f"(chance {num(100 * (m.get('chance_rate') or 0), 0, '%')}, "
                    f"p = {num(m.get('p_value'), 2)})",
                )
            )
    notes = "".join(f"<p class='note'>{esc(n)}</p>" for n in so.get("notes") or [])
    return (
        f"<h3>Social interaction <span class='note'>(A: {esc(a)}, B: {esc(b)})</span></h3>"
        + (table(rows) if rows else "")
        + notes
        + "<p class='note'>Synchrony: windowed cross-correlation, against surrogate pairs (one "
        "person's signal shifted in time). Mimicry: the other starts the same within 3 s.</p>"
    )


def stills_html(cam: CameraResults) -> str:
    if not cam.stills:
        return ""
    figs = "".join(
        "<figure><img alt='' src='data:image/jpeg;base64,"
        f"{base64.b64encode(jpg).decode('ascii')}'><figcaption>{clock(t)}</figcaption></figure>"
        for t, jpg in cam.stills
    )
    return (
        f"<div class='card'><div class='stills'>{figs}</div><p class='note'>Still frames: they "
        "show the participants; share this page only as their consent allows.</p></div>"
    )


# ── Sections ─────────────────────────────────────────────────────────────────


def table(rows: list[tuple]) -> str:
    body = "".join(f"<tr><th>{esc(k)}</th><td class='n'>{v}</td></tr>" for k, v in rows)
    return f"<table>{body}</table>"


def _face_dynamics(fd: dict) -> str:
    s = fd["summary"]
    b, e, h = s.get("blinks", {}), s.get("expression", {}), s.get("head", {})
    fps = fd.get("fps")
    note = (
        f"<p class='note'>Video at {num(fps)} fps: below 30 fps blink durations are coarse.</p>"
        if fps and fps < 30
        else ""
    )
    return (
        "<h3>Face Dynamics</h3>"
        + table(
            [
                ("Face seen", num(s.get("face_seen_pct"), 0, "%")),
                ("Blinks", f"{num(b.get('count'))} ({num(b.get('per_minute'), 1)}/min)"),
                ("Median blink", num(b.get("median_duration_ms"), 0, " ms")),
                ("Long eye closures", num(b.get("long_closures"))),
                ("PERCLOS", num(b.get("perclos_pct"), 1, "%")),
                ("Smiles (Duchenne)", f"{num(e.get('smiles'))} ({num(e.get('duchenne_smiles'))})"),
                ("Time smiling", num(e.get("smiling_pct"), 1, "%")),
                (
                    "Brow raises (flashes)",
                    f"{num(e.get('brow_raises'))} ({num(e.get('brow_flashes'))})",
                ),
                ("Expressivity", num(e.get("expressivity_mean"), 3)),
                ("Nods / shakes", f"{num(h.get('nods'))} / {num(h.get('shakes'))}"),
            ]
        )
        + note
    )


def _conversation(c: dict) -> str:
    s = c["summary"]
    sub, oth = s.get("speakers", {}).get("subject", {}), s.get("speakers", {}).get("other", {})
    tr, ov = s.get("transitions", {}), s.get("overlap", {})

    def resp(x):
        v = (x.get("response_offset") or {}).get("median_s")
        return num(None if v is None else 1000 * v, 0, " ms")

    rows = [
        ("", "<b>On camera</b>", "<b>Other</b>"),
        (
            "Speech",
            f"{num(sub.get('speech_s'), 0, ' s')} ({num(sub.get('speech_pct'), 0, '%')})",
            f"{num(oth.get('speech_s'), 0, ' s')} ({num(oth.get('speech_pct'), 0, '%')})",
        ),
        (
            "Turns (median length)",
            f"{num(sub.get('turns'))} "
            f"({num((sub.get('turn_duration') or {}).get('median_s'), 1, ' s')})",
            f"{num(oth.get('turns'))} "
            f"({num((oth.get('turn_duration') or {}).get('median_s'), 1, ' s')})",
        ),
        ("Response time (median)", resp(sub), resp(oth)),
        ("Interruptions made", num(sub.get("interruptions")), num(oth.get("interruptions"))),
        ("Pauses per minute", num(sub.get("pauses_per_min"), 1), num(oth.get("pauses_per_min"), 1)),
        ("Backchannels", num(sub.get("backchannels")), num(oth.get("backchannels"))),
        ("Words per minute", num(sub.get("words_per_min"), 0), num(oth.get("words_per_min"), 0)),
    ]
    body = "".join(
        f"<tr><th>{esc(a)}</th><td class='n'>{b}</td><td class='n'>{c_}</td></tr>"
        for a, b, c_ in rows
    )
    method = (c.get("attribution") or {}).get("method")
    timing = (c.get("timing") or {}).get("method")
    note = (
        f"<p class='note'>{num(tr.get('count'))} changes of speaker: {num(tr.get('gaps'))} gaps, "
        f"{num(tr.get('overlapping'))} overlapping, {num(tr.get('interruptions'))} interruptions; "
        f"overlap {num(ov.get('pct_of_speech'), 1, '%')} of speech. Speakers from "
        f"{'diarization and mouth movement' if method == 'diarization' else 'mouth movement only'}"
        f"{'' if timing == 'timing_file' else APPROX_AUDIO}.</p>"
    )
    return f"<h3>Conversation Timing</h3><table>{body}</table>{note}"


def _eye_contact(ec: dict) -> str:
    s, t = ec["summary"], ec.get("target", {})
    dirs = s.get("aversion_directions") or {}
    turns = s.get("turns") or {}
    rows = [
        ("Eye contact", num(s.get("eye_contact_pct"), 0, "%")),
        (
            "While listening / speaking",
            f"{num(s.get('eye_contact_listening_pct'), 0, '%')} / "
            f"{num(s.get('eye_contact_speaking_pct'), 0, '%')}",
        ),
        (
            "Look-aways",
            f"{num(s.get('aversions'))} ({num(s.get('aversions_per_min'), 1)}/min, median "
            f"{num(s.get('aversion_median_s'), 1, ' s')})",
        ),
        (
            "Directions (up, down, left, right)",
            f"{num(dirs.get('up'))}, {num(dirs.get('down'))}, {num(dirs.get('left'))}, "
            f"{num(dirs.get('right'))}",
        ),
    ]
    if turns:
        rows += [
            (
                "Answers starting with a look-away",
                num(turns.get("turn_start_aversion_pct"), 0, "%"),
            ),
            ("Answers ending with eye contact", num(turns.get("turn_end_contact_pct"), 0, "%")),
        ]
    why = (
        "<p class='note'>While listening / speaking: run Eye Contact again after Conversation "
        "Timing (it needs who speaks when).</p>"
        if s.get("eye_contact_listening_pct") is None
        else ""
    )
    where = {"auto": "found from the gaze", "camera": "the camera", "manual": "given"}.get(
        t.get("method"), ""
    )
    cone = num(t.get("radius_deg"), 0, "°")
    note = f"<p class='note'>Partner: {esc(where)}; contact cone {cone}.</p>"
    return "<h3>Eye Contact</h3>" + table(rows) + note + why


def _rppg(hr: dict) -> str:
    s, h = hr["summary"], hr.get("hrv")
    rows = [
        (
            "Heart rate (mean, range)",
            f"{num(s.get('mean_bpm'), 0, ' bpm')} "
            f"({num(s.get('min_bpm'), 0)}–{num(s.get('max_bpm'), 0)})",
        ),
        (
            "Windows with an estimate",
            num(None if s.get("pct_windows_good") is None else 100 * s["pct_windows_good"], 0, "%"),
        ),
    ]
    if h:
        rows += [
            (
                "RMSSD (noise-corrected)",
                f"{num(h.get('rmssd_ms'), 0, ' ms')} "
                f"({num(h.get('rmssd_corrected_ms'), 0, ' ms')})",
            ),
            (
                "SDNN (noise-corrected)",
                f"{num(h.get('sdnn_ms'), 0, ' ms')} ({num(h.get('sdnn_corrected_ms'), 0, ' ms')})",
            ),
            ("Beat-timing noise", num(h.get("timing_jitter_ms"), 1, " ms")),
            ("Clean beats", num(h.get("nn_count"))),
        ]
    by_raw = hr.get("by_state")
    by = by_raw or {}
    speaking = (by.get("speaking") or {}).get("mean_hr_bpm")
    listening = (by.get("listening") or {}).get("mean_hr_bpm")
    if speaking is None and listening is None:
        by = {}
    if by:
        rows.append(
            (
                "Heart rate speaking / listening",
                f"{num((by.get('speaking') or {}).get('mean_hr_bpm'), 0)} / "
                f"{num((by.get('listening') or {}).get('mean_hr_bpm'), 0)} bpm",
            )
        )
    withheld = hr.get("hrv_withheld") or []
    if not by:
        rows.append(("Heart rate speaking / listening", "–"))
    note = (
        "<p class='note'>Experimental: not a medical device, not validated against a reference.</p>"
    )
    if not h and withheld:
        note = f"<p class='note'>HRV not reported: {esc('; '.join(withheld))}.</p>" + note
    if not by:
        why = (
            "run Remote Heart Rate again after Conversation Timing (it needs who speaks when)"
            if by_raw is None
            else "fewer than 10 clean beats while speaking and while listening"
        )
        note = f"<p class='note'>Speaking / listening: {why}.</p>" + note
    return f"<h3>Heart rate and HRV ({esc(hr.get('backend') or '')})</h3>" + table(rows) + note


def _small(name: str, rows: list) -> str:
    return f"<h3>{esc(name)}</h3>" + table(rows)


def _objects(ob: dict) -> str:
    rows = []
    for o in ob.get("objects", []):
        detail = f"seen {num(o.get('seen_pct'), 0, '%')}"
        if ob.get("pose_used") or ob.get("hands_followed"):
            who = o.get("touched_s_by_person") or {}
            # "?": a hand Pose did not place, on a camera that sees only an arm.
            detail += (
                "; touched by "
                + ", ".join(
                    f"{'someone' if p == '?' else 'person ' + esc(p)} {num(s, 1, ' s')}"
                    for p, s in who.items()
                )
                if who
                else "; not touched"
            )
        # A name given in the app, with what was found ("red ball (toy 2)").
        found = f"{o.get('label')} {o.get('id')}"
        rows.append((f"{o['name']} ({found})" if o.get("name") else found, detail))
    if not rows:
        rows = [("Found", "none of " + ", ".join(ob.get("labels", [])))]
    note = (
        ""
        if ob.get("pose_used")
        else "<p class='note'>Run Pose on this camera to see whose hand it is.</p>"
    )
    return _small("Objects (SAM 3)", rows) + note


def _hands(hd: dict) -> str:
    rows = []
    for h in hd.get("hands", []):
        who = "someone" if h.get("subject_id") is None else f"person {esc(h['subject_id'])}"
        side = "" if h.get("side") == "unknown" else f" {esc(h.get('side'))}"
        detail = (
            f"seen {num(h.get('seen_ms', 0) / 1000.0, 1, ' s')}, "
            f"about {num(h.get('median_size_px'), 0, ' px')}"
        )
        k = h.get("kinematics")
        if k:
            detail += (
                f"; wrist speed median {num(k.get('wrist_speed_median_hl_s'), 1)}, "
                f"95th percentile {num(k.get('wrist_speed_p95_hl_s'), 1)} hand lengths/s; "
                f"grip aperture median {num(k.get('grip_aperture_median_palm'), 2)} palm lengths"
            )
        mv = h.get("movements")
        if mv:
            cut = f" ({mv['cut_off']} cut off, left out)" if mv.get("cut_off") else ""
            detail += (
                f"; {num(mv.get('movements'))} movements{cut}, median peak "
                f"{num(mv.get('peak_speed_median_hl_s'), 1)} hand lengths/s, "
                f"{num(mv.get('movement_units_median'), 0)} units, "
                f"SPARC {num(mv.get('sparc_median'), 2)}"
            )
        rows.append((f"{who}{side} hand", detail))
    for p in hd.get("people", []):
        rows.append(
            (
                f"person {esc(p.get('subject_id'))}, both hands",
                f"laterality {num(p.get('laterality_index'), 2)} (-1 all left, 1 all right); "
                f"both moving {num(p.get('both_moving_s'), 1, ' s')}",
            )
        )
    model = "MediaPipe" if hd.get("model") == "mediapipe" else "RTMW"
    return _small(f"Hands ({model})", rows or [("Found", "no hand")])


def found_nothing(cam: CameraResults) -> bool:
    """Whether every analysis of this camera ran but saw no face or speech."""
    r = cam.results
    fd = (r.get("face_dynamics") or {}).get("summary") or {}
    checks = []
    if "face_dynamics" in r:
        checks.append(not fd.get("face_seen_pct"))
    if "eye_contact" in r:
        checks.append(r["eye_contact"]["summary"].get("eye_contact_pct") is None)
    if "conversation" in r:
        checks.append(not r["conversation"]["summary"].get("transitions", {}).get("count"))
    if "rppg" in r:
        checks.append(r["rppg"]["summary"].get("mean_bpm") is None)
    other = set(r) - {"face_dynamics", "eye_contact", "conversation", "rppg"}
    return bool(checks) and all(checks) and not other


def camera_heading(cam: CameraResults) -> str:
    model = (
        f" <span class='note'>{esc(cam.model)}</span>"
        if cam.model and cam.model != cam.name
        else ""
    )
    return f"<h2 id='cam{cam.index}'>{esc(cam.name)}{model}</h2>"


def camera_section(cam: CameraResults) -> str:
    r = cam.results
    parts = [camera_heading(cam), stills_html(cam)]
    svg = timeline_svg(cam)
    if svg:
        parts.append(f'<div class="card">{svg}{legend(cam)}</div>')
    body = []
    if "face_dynamics" in r:
        body.append(_face_dynamics(r["face_dynamics"]))
    if "conversation" in r:
        body.append(_conversation(r["conversation"]))
    if "eye_contact" in r:
        body.append(_eye_contact(r["eye_contact"]))
    if "rppg" in r:
        body.append(_rppg(r["rppg"]))
    if "gaze2d" in r:
        s = r["gaze2d"]["summary"]
        on = s.get("pct_on_target")
        body.append(
            _small(
                "2D Gaze",
                [
                    (
                        "Frames with a face",
                        num(
                            None
                            if s.get("pct_frames_with_face") is None
                            else 100 * s["pct_frames_with_face"],
                            0,
                            "%",
                        ),
                    ),
                    ("Looking at the camera", num(None if on is None else 100 * on, 0, "%")),
                ],
            )
        )
    if "expression" in r:
        ex = r["expression"]
        rows = [(k, num(v, 0, "%")) for k, v in list(ex["shares_pct"].items())[:6]]
        body.append(
            _small(
                f"Facial Expression ({ex.get('backend')})", rows or [("Frames with a face", "0")]
            )
        )
    if "pose" in r:
        p = r["pose"]
        body.append(
            _small(
                f"Pose ({p.get('model')})",
                [
                    ("Frames with a person", num(p.get("frames_with_person_pct"), 0, "%")),
                    ("People tracked (in 5% of frames or more)", num(p.get("tracked_people"))),
                ],
            )
        )
    if "objects" in r:
        body.append(_objects(r["objects"]))
    if "hands" in r:
        body.append(_hands(r["hands"]))
    charts = [
        c
        for c in (
            _response_chart(r["conversation"]) if "conversation" in r else None,
            _eye_contact_by_state(r["eye_contact"]) if "eye_contact" in r else None,
        )
        if c
    ]
    parts.append(
        '<div class="grid">'
        + "".join(f'<div class="card">{b}</div>' for b in body + charts)
        + "</div>"
    )
    return "".join(parts)


def glance(data: SessionData) -> str:
    """Headline numbers of the camera with the most results."""

    def score(c):
        seen = ((c.results.get("face_dynamics") or {}).get("summary") or {}).get(
            "face_seen_pct"
        ) or 0
        return (not found_nothing(c), len(c.results), seen)

    best = max((c for c in data.cameras if c.results), key=score, default=None)
    if best is None or found_nothing(best):
        return ""
    r = best.results
    tiles = []

    def add(value, label):
        if value not in (None, "–"):
            tiles.append(
                f"<div class='tile'><div class='v'>{value}</div><div "
                f"class='k'>{esc(label)}</div></div>"
            )

    fd = (r.get("face_dynamics") or {}).get("summary") or {}
    add(num((fd.get("blinks") or {}).get("per_minute"), 1), "blinks per minute")
    add(num((fd.get("expression") or {}).get("smiling_pct"), 0, "%"), "of the time smiling")
    ec = (r.get("eye_contact") or {}).get("summary") or {}
    add(num(ec.get("eye_contact_pct"), 0, "%"), "eye contact")
    conv = (r.get("conversation") or {}).get("summary") or {}
    subj = (conv.get("speakers") or {}).get("subject") or {}
    med = (subj.get("response_offset") or {}).get("median_s")
    add(num(None if med is None else 1000 * med, 0, " ms"), "median response time")
    add(num(subj.get("speech_pct"), 0, "%"), "of the talking (on camera)")
    hr = r.get("rppg") or {}
    add(num((hr.get("summary") or {}).get("mean_bpm"), 0, " bpm"), "mean heart rate")
    h = hr.get("hrv") or {}
    rmssd = h["rmssd_corrected_ms"] if "rmssd_corrected_ms" in h else h.get("rmssd_ms")
    add(num(rmssd, 0, " ms"), "RMSSD (noise-corrected)" if "rmssd_corrected_ms" in h else "RMSSD")
    if not tiles:
        return ""
    head = f"<h2 id='glance'>At a glance <span class='note'>({esc(best.name)})</span></h2>"
    return f"{head}<div class='tiles'>{''.join(tiles)}</div>"


def session_sections(data: SessionData) -> str:
    out = []
    tr = data.session_results.get("transcripts")
    if tr:
        rows = []
        for t in tr:
            for who, v in t["speakers"].items():
                rows.append(
                    f"<tr><td>{esc(t['audio'])}</td><td>{esc(who)}</td><td "
                    f"class='n'>{v['segments']}</td>"
                    f"<td class='n'>{v['words']}</td><td "
                    f"class='n'>{num(v['seconds'], 0, ' s')}</td></tr>"
                )
        out.append(
            "<h3>Transcript</h3><table><tr><th>Audio</th><th>Speaker</th><th "
            "class='n'>Segments</th>"
            f"<th class='n'>Words</th><th class='n'>Time</th></tr>{''.join(rows)}</table>"
        )
        for t in tr:
            lines = "".join(
                f"<tr><td>{clock(ms / 1000.0)}</td><td>{esc(who)}</td><td>{esc(text)}</td></tr>"
                for ms, who, text in t.get("lines", [])
                if text
            )
            if lines:
                out.append(
                    f"<p class='note'>{esc(t['audio'])}, as transcribed (times from the start "
                    f"of the audio):</p><div class='scroll'><table>{lines}</table></div>"
                )
    vo = data.session_results.get("voice")
    if vo:
        out.append(
            _small(
                "Voice",
                [
                    (
                        v["audio"],
                        f"median pitch {num(v['median_pitch_hz'], 0, ' Hz')}, voiced "
                        f"{num(v['voiced_pct'], 0, '%')}, "
                        f"intensity {num(v['mean_intensity_db'], 0, ' dB')}",
                    )
                    for v in vo
                ],
            )
        )
        for v in vo:
            svg = voice_svg(v)
            if svg:
                out.append(
                    svg + "<p class='note'>" + swatch("--l-smile", "pitch (right scale)") + "Grey: "
                    "the spectrogram, frequency on the left (brighter is louder), time below.</p>"
                )
    gz = data.session_results.get("gaze_fusion")
    if gz:
        rows = []
        for name, s in gz["summary"].get("subjects", {}).items():
            top = ", ".join(
                f"{esc(k)} {num(v, 0, ' s')}" for k, v in list(s.get("looked_at_s", {}).items())[:4]
            )
            rows.append((name, f"seen {num(s.get('seen_s'), 0, ' s')}; looked at: {top or '–'}"))
        for pair, v in gz["summary"].get("mutual_gaze", {}).items():
            rows.append(
                (
                    f"Mutual gaze {pair}",
                    f"{num(v.get('seconds'), 0, ' s')} ({num(v.get('pct_of_recording'), 1, '%')})",
                )
            )
        out.append(_small("3D Gaze", rows))
    p3 = data.session_results.get("pose3d")
    if p3:
        ident = (p3.get("quality") or {}).get("identity") or {}
        out.append(
            _small(
                "3D Pose",
                [
                    ("People tracked", num(ident.get("track_count"))),
                    ("Tracks under 1 s", num(ident.get("short_tracks"))),
                ],
            )
        )
        strip = coverage_svg((p3.get("quality") or {}).get("timeline") or {})
        if strip:
            out.append(strip)
    # Also when skeleton3d.json's quality header could not be read.
    if p3 or data.session_results.get("pose3d_people"):
        out.append(_pose3d_people_rows(data.session_results.get("pose3d_people")))
    so = data.session_results.get("social")
    if so:
        out.append(social_html(so))
    sy = data.session_results.get("sync_repair")
    if sy:
        cams = sy.get("cameras") or []
        skipped = sum(1 for c in cams if c.get("skipped"))
        out.append(
            _small(
                "Frame sync",
                [
                    ("Alignment", esc(sy.get("alignment"))),
                    ("Rate", num(sy.get("fps"), 2, " fps")),
                    ("Cameras synced / skipped", f"{len(cams) - skipped} / {skipped}"),
                ],
            )
        )
    if not out:
        return ""
    return "<h2 id='session'>Whole session</h2><div class='card'>" + "".join(out) + "</div>"


def _pose3d_people_rows(pp: dict | None) -> str:
    """Each lasting person's posture, joint angles and movement, from
    pose3d_people.json; a note when MOSAIC has not written it yet."""
    if not pp:
        return _small(
            "3D Pose, per person",
            [("Not summarised yet", "open this session's 3D Pose result in MOSAIC once")],
        )
    rows = []
    for p in pp["people"]:
        if p.get("brief"):
            continue  # under a second in 3D: hidden in MOSAIC too
        post = p.get("seconds_by_posture") or {}
        posture = ", ".join(
            f"{k} {num(post.get(k), 0, ' s')}"
            for k in ("sitting", "standing", "walking")
            if post.get(k)
        )
        move = (p.get("movement") or {}).get("motion_m_s") or {}
        knees = ((p.get("median_angles_deg") or {}).get("knee_flexion")) or [None, None]
        parts = [f"in 3D {num(p.get('seconds_in_3d'), 0, ' s')}"]
        if posture:
            parts.append(posture)
        if move.get("median") is not None:
            parts.append(
                f"moving {num(move.get('median'), 2, ' m/s')} (high {num(move.get('p95'), 2)})"
            )
        if any(k is not None for k in knees):
            parts.append(f"knees usually {num(knees[0], 0, '°')} / {num(knees[1], 0, '°')}")
        rows.append((esc(p.get("label")), "; ".join(parts)))
    if pp.get("stale"):
        rows.append(("Note", "older than the 3D Pose result: open it in MOSAIC to update"))
    return _small("3D Pose, per person", rows) if rows else ""


def not_run(data: SessionData) -> str:
    missing = []
    for key, label in PER_CAMERA_ANALYSES:
        if data.cameras and not any(key in c.results for c in data.cameras):
            missing.append(label)
    single = len(data.cameras) < 2
    for key, label in SESSION_ANALYSES:
        if key in ("gaze_fusion", "pose3d", "social") and single:
            continue  # needs several calibrated cameras
        if key not in data.session_results:
            missing.append(label)
    if not missing:
        return ""
    items = "".join(f"<li>{esc(m)}</li>" for m in missing)
    return f"<h2 id='notrun'>Not run on this session</h2><ul class='missing'>{items}</ul>"


def render(data: SessionData) -> str:
    meta = data.meta
    bids = meta.get("bids") or {}
    rec = meta.get("recording") or {}
    title = data.path.name
    ident = " · ".join(
        f"{k} {esc(v)}"
        for k, v in (
            ("subject", bids.get("sub")),
            ("session", bids.get("ses")),
            ("task", bids.get("task")),
            ("run", bids.get("run")),
        )
        if v not in (None, "")
    )
    dur = duration_s(data)
    chips = [
        esc(meta.get("session_start_utc", "")).replace("T", " ")[:19] + " UTC"
        if meta.get("session_start_utc")
        else "",
        f"{esc(rec.get('mode', 'room'))} mode",
        f"{clock(dur)} long" if dur else "",
        f"{len(data.cameras)} camera{'' if len(data.cameras) == 1 else 's'}",
        f"recorded by {esc(meta.get('recorded_by'))}" if meta.get("recorded_by") else "",
    ]
    end = meta.get("session_end")
    if "session_end" in meta and end is None:
        # Written as null at the start and replaced at a clean stop.
        chips.append("recording did not finish (interrupted, or still running)")
    elif isinstance(end, dict) and not end.get("ended_cleanly", True):
        chips.append("did not end cleanly")
    notes = (
        f"<div class='card'><h3>Notes</h3><div class='notes'>{esc(data.notes)}</div></div>"
        if data.notes
        else ""
    )
    shown = [c for c in data.cameras if c.results and not found_nothing(c)]
    empty = [c for c in data.cameras if c.results and found_nothing(c)]
    cams = "".join(camera_section(c) for c in shown)
    glance_html, quality_html = glance(data), quality_section(data)
    whole, missing = session_sections(data), not_run(data)
    links = [
        ("glance", "At a glance", glance_html),
        ("quality", "Data quality", quality_html),
        *[(f"cam{c.index}", c.name, "x") for c in shown],
        ("session", "Whole session", whole),
        ("notrun", "Not run", missing),
    ]
    toc = "".join(f"<a href='#{i}'>{esc(t)}</a>" for i, t, body in links if body)
    toc = f"<nav class='toc'>{toc}</nav>" if toc else ""
    if empty:
        names = ", ".join(c.name for c in empty)
        cams += (
            f"<p class='note'>{esc(names)}: analysed, but no face (or conversation) was found.</p>"
        )
    return (
        "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        f"<title>Session report {esc(title)}</title><style>{CSS}</style></head><body><main>"
        f"<h1>Session report</h1><p class='sub'>{esc(title)}{(' · ' + ident) if ident else ''}</p>"
        f"<div class='chips'>{''.join(f'<span>{c}</span>' for c in chips if c)}</div>"
        f"{toc}{notes}{glance_html}{quality_html}{cams}{whole}{missing}"
        f"<p class='note' style='margin-top:32px'>Made by MOSAIC on "
        f"{time.strftime('%Y-%m-%d %H:%M')} from the analysis results in this session's folder. "
        "Rerun the report after running more analyses.</p>"
        "</main></body></html>"
    )
