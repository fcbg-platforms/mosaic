"""
The session report as one self-contained HTML page: inline CSS and SVG, no
scripts, no external files, readable in light and dark themes, and printable
(the browser's "Save as PDF" gives a clean document).
"""

from __future__ import annotations

import html
import math
import time

from .collect import PER_CAMERA_ANALYSES, SESSION_ANALYSES, CameraResults, SessionData, duration_s

CSS = """
:root {
  --bg: #f7f7f9; --card: #ffffff; --ink: #1d1d27; --muted: #63637a; --line: #e2e2ea;
  --accent: #3d5afe; --a: #2e9d5b; --b: #2f80d1; --c: #d08a1a; --d: #c2410c; --e: #8b5cf6;
  --grid: #ececf2;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --bg: #12121a; --card: #1b1b26; --ink: #e8e8f0; --muted: #9a9ab0; --line: #2c2c3c;
    --accent: #8c9eff; --a: #4cc584; --b: #5aa6f0; --c: #f0b44c; --d: #f08a5d; --e: #b39bff;
    --grid: #262634;
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
@media print { body { background: #fff; } .card, .tile { break-inside: avoid; } }
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


def timeline_svg(cam: CameraResults) -> str | None:
    """Time-aligned lanes of everything measured on one camera."""
    r = cam.results
    if cam.t0_ms is None:  # no timestamps file: times cannot be placed on the video
        return None
    lanes: list[tuple[str, list]] = []  # (label, [(t0_s, t1_s, colour var) or (t, None, colour)])
    t0 = cam.t0_ms

    def rel(ms):
        return (ms - t0) / 1000.0

    times = []
    conv = r.get("conversation")
    if conv:
        for who, label, colour in (
            ("subject", "On camera speaks", "--a"),
            ("other", "Other speaks", "--b"),
        ):
            spans = [
                (rel(s["start_ms"]), rel(s["end_ms"]), colour)
                for s in conv["spurts"]
                if s["speaker"] == who
            ]
            lanes.append((label, spans))
    ec = r.get("eye_contact")
    if ec:
        spans = [(rel(a), rel(b), "--a" if c else "--c") for a, b, c in ec["runs"]]
        lanes.append(("Eye contact / away", spans))
    fd = r.get("face_dynamics")
    if fd:
        ev = fd["events"]
        lanes.append(
            ("Blinks", [(rel(e["peak_ms"]), None, "--b") for e in ev if e["kind"] == "blink"])
        )
        duch = {e["start_ms"] for e in ev if e["kind"] == "duchenne_smile"}
        lanes.append(
            (
                "Smiles",
                [
                    (
                        rel(e["start_ms"]),
                        rel(e["end_ms"]),
                        "--d" if e["start_ms"] in duch else "--c",
                    )
                    for e in ev
                    if e["kind"] == "smile"
                ],
            )
        )
        lanes.append(
            (
                "Brow raises",
                [(rel(e["peak_ms"]), None, "--e") for e in ev if e["kind"] == "brow_raise"],
            )
        )
        lanes.append(
            (
                "Nods / shakes",
                [
                    (rel(e["start_ms"]), rel(e["end_ms"]), "--a" if e["kind"] == "nod" else "--d")
                    for e in ev
                    if e["kind"] in ("nod", "shake")
                ],
            )
        )
    hr_points = []
    hr = r.get("rppg")
    if hr:
        for w in hr.get("windows", []):
            bpm = w.get("smoothed_bpm") or w.get("bpm")
            if bpm is not None:
                hr_points.append(((w["start_ms"] + w["end_ms"]) / 2.0, bpm))
    lanes = [(label, items) for label, items in lanes if items]
    if not lanes and not hr_points:
        return None
    for _, items in lanes:
        for a, b, _c in items:
            times.extend([a] + ([b] if b is not None else []))
    times.extend(rel(t) for t, _ in hr_points)
    # The whole video, so every camera's lanes line up with its own length.
    t_max = max([t for t in times if t is not None] + [cam.duration_s or 0.0, 1.0])
    plot_w = CHART_W - LABEL_W - 10
    x = lambda t: LABEL_W + max(0.0, min(t, t_max)) / t_max * plot_w  # noqa: E731
    hr_h = 70 if hr_points else 0
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
        for a, b, colour in items:
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
                    f'fill="var({colour})" rx="2"/>'
                )
        y += LANE_H + LANE_GAP
    if hr_points:
        vals = [v for _, v in hr_points]
        lo, hi = min(vals), max(vals)
        if hi - lo < 10:
            mid = (hi + lo) / 2.0
            lo, hi = mid - 5, mid + 5
        top, bottom = y + 6, y + hr_h - 6
        yy = lambda v: bottom - (v - lo) / (hi - lo) * (bottom - top)  # noqa: E731
        pts = " ".join(f"{x(rel(t)):.1f},{yy(v):.1f}" for t, v in hr_points)
        out.append(f'<text x="{LABEL_W - 8}" y="{top + 10}" text-anchor="end">Heart rate</text>')
        out.append(
            f'<text x="{LABEL_W - 8}" y="{top + 24}" text-anchor="end">{lo:.0f}–{hi:.0f} bpm</text>'
        )
        out.append(f'<polyline points="{pts}" fill="none" stroke="var(--d)" stroke-width="1.8"/>')
    out.append("</svg>")
    return "".join(out)


def legend() -> str:
    items = [
        ("--a", "on camera speaks, eye contact, nod"),
        ("--b", "other speaks, blink"),
        ("--c", "looking away, smile"),
        ("--d", "Duchenne smile, head shake, heart rate"),
        ("--e", "brow raise"),
    ]
    parts = [
        f'<span style="display:inline-flex;align-items:center;margin-right:14px">'
        "<span "
        f'style="width:12px;height:12px;border-radius:3px;background:var({c});margin-right:6px"></span>'
        f"{esc(t)}</span>"
        for c, t in items
    ]
    return f'<p class="note">{"".join(parts)}</p>'


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
    where = {"auto": "found from the gaze", "camera": "the camera", "manual": "given"}.get(
        t.get("method"), ""
    )
    cone = num(t.get("radius_deg"), 0, "°")
    note = f"<p class='note'>Partner: {esc(where)}; contact cone {cone}.</p>"
    return "<h3>Eye Contact</h3>" + table(rows) + note


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
    by = hr.get("by_state") or {}
    if by:
        rows.append(
            (
                "Heart rate speaking / listening",
                f"{num((by.get('speaking') or {}).get('mean_hr_bpm'), 0)} / "
                f"{num((by.get('listening') or {}).get('mean_hr_bpm'), 0)} bpm",
            )
        )
    withheld = hr.get("hrv_withheld") or []
    note = (
        "<p class='note'>Experimental: not a medical device, not validated against a reference.</p>"
    )
    if not h and withheld:
        note = f"<p class='note'>HRV not reported: {esc('; '.join(withheld))}.</p>" + note
    return f"<h3>Heart rate and HRV ({esc(hr.get('backend') or '')})</h3>" + table(rows) + note


def _small(name: str, rows: list) -> str:
    return f"<h3>{esc(name)}</h3>" + table(rows)


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
    model = f" <span class='note'>{esc(cam.model)}</span>" if cam.model else ""
    return f"<h2>{esc(cam.name)}{model}</h2>"


def camera_section(cam: CameraResults) -> str:
    r = cam.results
    parts = [camera_heading(cam)]
    svg = timeline_svg(cam)
    if svg:
        parts.append(f'<div class="card">{svg}{legend()}</div>')
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
    parts.append(
        '<div class="grid">' + "".join(f'<div class="card">{b}</div>' for b in body) + "</div>"
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
    head = f"<h2>At a glance <span class='note'>({esc(best.name)})</span></h2>"
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
    return "<h2>Whole session</h2><div class='card'>" + "".join(out) + "</div>"


def not_run(data: SessionData) -> str:
    missing = []
    for key, label in PER_CAMERA_ANALYSES:
        if data.cameras and not any(key in c.results for c in data.cameras):
            missing.append(label)
    for key, label in SESSION_ANALYSES:
        if key not in data.session_results:
            missing.append(label)
    if not missing:
        return ""
    items = "".join(f"<li>{esc(m)}</li>" for m in missing)
    return f"<h2>Not run on this session</h2><ul class='missing'>{items}</ul>"


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
        f"{notes}{glance(data)}{cams}{session_sections(data)}{not_run(data)}"
        f"<p class='note' style='margin-top:32px'>Made by MOSAIC on "
        f"{time.strftime('%Y-%m-%d %H:%M')} from the analysis results in this session's folder. "
        "Rerun the report after running more analyses.</p>"
        "</main></body></html>"
    )
