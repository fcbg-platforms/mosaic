"""
One flat row of numbers per session, for statistics across sessions.

Column names are stable: the same analysis always gives the same column. With
one recorded camera (interview mode) the per-camera columns carry no prefix,
so interviews with different camera numbers line up; with several cameras
each column is prefixed ``camN_`` (N counting from 1, as in the app). 3D gaze
columns use subject ids (``S1``), not display names.
"""

from __future__ import annotations

import csv
import re
from pathlib import Path

from .collect import SessionData, duration_s


def _get(d, *keys):
    for k in keys:
        if not isinstance(d, dict):
            return None
        d = d.get(k)
    return d


def camera_row(results: dict) -> dict:
    """The per-camera numbers (unprefixed)."""
    row: dict = {}
    fd = results.get("face_dynamics")
    if fd:
        s = fd["summary"]
        row.update(
            {
                "fd_face_seen_pct": s.get("face_seen_pct"),
                "fd_blinks_per_min": _get(s, "blinks", "per_minute"),
                "fd_blink_median_ms": _get(s, "blinks", "median_duration_ms"),
                "fd_perclos_pct": _get(s, "blinks", "perclos_pct"),
                "fd_smiles": _get(s, "expression", "smiles"),
                "fd_duchenne_smiles": _get(s, "expression", "duchenne_smiles"),
                "fd_smiling_pct": _get(s, "expression", "smiling_pct"),
                "fd_brow_raises": _get(s, "expression", "brow_raises"),
                "fd_expressivity": _get(s, "expression", "expressivity_mean"),
                "fd_nods": _get(s, "head", "nods"),
                "fd_shakes": _get(s, "head", "shakes"),
            }
        )
    conv = results.get("conversation")
    if conv:
        s = conv["summary"]
        subj = _get(s, "speakers", "subject") or {}
        oth = _get(s, "speakers", "other") or {}

        def ms(v):
            return None if v is None else round(1000.0 * v)

        row.update(
            {
                "conv_subject_speech_pct": subj.get("speech_pct"),
                "conv_other_speech_pct": oth.get("speech_pct"),
                "conv_subject_turns": subj.get("turns"),
                "conv_subject_response_ms": ms(_get(subj, "response_offset", "median_s")),
                "conv_other_response_ms": ms(_get(oth, "response_offset", "median_s")),
                "conv_subject_pauses_per_min": subj.get("pauses_per_min"),
                "conv_subject_words_per_min": subj.get("words_per_min"),
                "conv_transitions": _get(s, "transitions", "count"),
                "conv_interruptions": _get(s, "transitions", "interruptions"),
                "conv_overlap_pct": _get(s, "overlap", "pct_of_speech"),
                "conv_attribution": _get(conv, "attribution", "method"),
            }
        )
    ec = results.get("eye_contact")
    if ec:
        s = ec["summary"]
        row.update(
            {
                "ec_contact_pct": s.get("eye_contact_pct"),
                "ec_listening_pct": s.get("eye_contact_listening_pct"),
                "ec_speaking_pct": s.get("eye_contact_speaking_pct"),
                "ec_aversions_per_min": s.get("aversions_per_min"),
                "ec_aversion_median_s": s.get("aversion_median_s"),
                "ec_turn_start_aversion_pct": _get(s, "turns", "turn_start_aversion_pct"),
                "ec_turn_end_contact_pct": _get(s, "turns", "turn_end_contact_pct"),
                "ec_partner": _get(ec, "target", "method"),
            }
        )
    hr = results.get("rppg")
    if hr:
        h = hr.get("hrv") or {}
        row.update(
            {
                "hr_mean_bpm": _get(hr, "summary", "mean_bpm"),
                "hrv_rmssd_ms": h.get("rmssd_ms"),
                "hrv_rmssd_corrected_ms": h.get("rmssd_corrected_ms"),
                "hrv_sdnn_ms": h.get("sdnn_ms"),
                "hrv_timing_noise_ms": h.get("timing_jitter_ms"),
                "hr_speaking_bpm": _get(hr, "by_state", "speaking", "mean_hr_bpm"),
                "hr_listening_bpm": _get(hr, "by_state", "listening", "mean_hr_bpm"),
            }
        )
    g2 = results.get("gaze2d")
    if g2:
        on = _get(g2, "summary", "pct_on_target")
        row["gaze2d_on_target_pct"] = None if on is None else round(100.0 * on, 1)
    ex = results.get("expression")
    if ex and ex["shares_pct"]:
        top = next(iter(ex["shares_pct"].items()))
        row["expr_dominant"] = top[0]
        row["expr_dominant_pct"] = top[1]
    return row


def session_row(data: SessionData) -> dict:
    """The whole session's row."""
    meta = data.meta
    bids = meta.get("bids") or {}
    rec = meta.get("recording") or {}
    row = {
        "session": data.path.name,
        "subject": bids.get("sub"),
        "session_label": bids.get("ses"),
        "task": bids.get("task"),
        "run": bids.get("run"),
        "recorded_by": meta.get("recorded_by"),
        "start_utc": meta.get("session_start_utc"),
        "mode": rec.get("mode", "room"),
        "duration_s": duration_s(data),
    }
    cams = [c for c in data.cameras if c.results]
    prefix_cams = len(data.cameras) > 1
    for cam in cams:
        prefix = f"cam{cam.index + 1}_" if prefix_cams else ""
        row.update({prefix + k: v for k, v in camera_row(cam.results).items()})
    for t in data.session_results.get("transcripts") or []:
        row["transcript_words"] = (row.get("transcript_words") or 0) + t["words"]
    gz = data.session_results.get("gaze_fusion")
    if gz:
        # Columns by subject id (S1, S2, ...), not display names, which can
        # be renamed per session.
        ids = {name: s.get("id", name) for name, s in (gz["summary"].get("subjects") or {}).items()}
        for pair, v in (gz["summary"].get("mutual_gaze") or {}).items():
            a, _, b = pair.partition(" & ")
            sa, sb = sorted((str(ids.get(a, a)), str(ids.get(b, b))))
            row[f"gaze3d_mutual_{sa}_{sb}_pct"] = v.get("pct_of_recording")
    return row


def write_rows(path: Path, rows: list[dict]) -> None:
    """Rows to CSV, with the union of their columns (session columns first,
    then the rest in first-seen order)."""
    columns: list[str] = []
    for r in rows:
        for k in r:
            if k not in columns:
                columns.append(k)

    def group(name: str) -> int:
        # Session columns, then unprefixed per-camera ones, then cam1_, cam2_, ...
        m = re.match(r"cam(\d+)_", name)
        return int(m.group(1)) if m else 0

    columns.sort(key=group)  # stable: first-seen order within each group
    # utf-8-sig: Excel then reads names with accents correctly.
    with Path(path).open("w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=columns)
        w.writeheader()
        for r in rows:
            w.writerow({k: ("" if r.get(k) is None else r.get(k)) for k in columns})
