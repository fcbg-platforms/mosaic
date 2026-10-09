"""
MOSAIC Conversation Timing: who speaks when, turns, response times, pauses,
overlaps and backchannels, from a session's audio and the face on a camera.
See :mod:`conversation`.

    python analysis/run_conversation.py --session /path/to/session

For each camera the **subject** is the face that camera sees (in interview
mode, the interviewee); everyone else heard on the microphone is the
**other** speaker. Speaker Diarization output (``audio/<name>.transcript.json``)
is used when present, for speaker labels and the words of each turn.

Outputs, in ``<session>/conversation/`` per camera:

* ``video_N.conversation.json``: timing method, attribution, summary,
  turns, transitions, overlaps, and audio level / mouth movement series.
* ``video_N.turns.csv`` and ``video_N.transitions.csv``.
* ``video_N.conversation.mp4``: the video with who is speaking, the turn's
  words and a scrolling timeline drawn on it (``--no-video``: none). It has
  no sound track.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
import time
import traceback
from pathlib import Path

import numpy as np

TAG = "[run_conversation]"
SCHEMA = "mosaic-conversation-v1"
#: Grid of the speech and mouth analysis, seconds.
HOP_S = 0.01
#: Step of the series saved for the app's chart, milliseconds.
SIGNAL_STEP_MS = 50


def parse_args(argv=None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="MOSAIC conversation timing")
    p.add_argument("--session", metavar="DIR", required=True, help="Recorded session directory")
    p.add_argument("--camera", type=int, default=None, help="Only this camera index")
    p.add_argument(
        "--audio", default=None, help="Microphone file name in audio/ (default: the first)"
    )
    p.add_argument("--min-confidence", type=float, default=0.5, help="Face detection threshold")
    p.add_argument(
        "--no-diarization",
        action="store_true",
        help="Ignore speaker labels from Speaker Diarization",
    )
    p.add_argument("--no-video", action="store_true", help="Do not write the annotated video")
    return p.parse_args(argv)


def log(msg: str) -> None:
    print(f"{TAG} {msg}", flush=True)


def progress_line(done: int, total: int, t_start: float) -> None:
    pct = 100.0 * done / max(total, 1)
    print(
        f"  {pct:5.1f}%  ({done}/{total})  {time.perf_counter() - t_start:.1f}s elapsed", flush=True
    )


# ── Audio ────────────────────────────────────────────────────────────────────


class Audio:
    """One microphone: speech-band level and speech mask per hop, its clock,
    and its transcript."""

    def __init__(self, wav: Path, meta: dict, use_diarization: bool) -> None:
        from conversation import timing as tm
        from conversation.vad import band_energy_db, detect_speech, detect_speech_silero, read_wav

        self.path = wav
        x, self.rate = read_wav(wav)
        self.duration_s = len(x) / self.rate
        self.db = band_energy_db(x, self.rate, HOP_S)
        energy_mask, self.levels = detect_speech(self.db, HOP_S)
        silero = detect_speech_silero(x, self.rate, len(self.db), HOP_S)
        self.vad = "silero" if silero is not None else "energy"
        self.speech = silero if silero is not None else energy_mask
        self.hop_samples = HOP_S * self.rate

        clock = None
        timing_csv = wav.with_suffix(".timing.csv")
        if timing_csv.exists():
            clock = tm.fit_timing(*tm.read_timing_csv(timing_csv), self.rate)
            if clock is None:
                log(f"{timing_csv.name} could not be used; falling back to the session start.")
        if clock is None:
            start = meta.get("session_start_elapsed_ns")
            if start is None:
                raise RuntimeError("session_meta.json has no session_start_elapsed_ns")
            clock = tm.AudioClock(float(start), 1e9 / self.rate, "session_start")
        self.clock = clock

        self.segments: list = []
        self.diarized = False
        transcript = wav.with_suffix("").with_suffix(".transcript.json")
        if transcript.exists():
            doc = json.loads(transcript.read_text(encoding="utf-8"))
            self.segments = doc.get("segments", [])
            self.diarized = bool(doc.get("diarization")) and any(
                s.get("speaker") for s in self.segments
            )
            if not use_diarization:
                self.diarized = False

    def hop_time_ms(self, clock=None) -> np.ndarray:
        """Elapsed time (ms) of each hop's centre sample."""
        c = clock or self.clock
        return c.sample_time_ns(np.arange(len(self.db)) * self.hop_samples) / 1e6

    def on_grid(self, values, grid_ms, clock=None) -> np.ndarray:
        """Per-hop values at grid times (nearest hop; ``nan``, or false for
        a mask, outside the audio)."""
        c = clock or self.clock
        k = (np.asarray(grid_ms) * 1e6 - c.t0_ns) / c.ns_per_sample / self.hop_samples
        idx = np.rint(k).astype(np.int64)
        v = np.asarray(values)
        inside = (idx >= 0) & (idx < len(v))
        out = np.zeros(len(idx), bool) if v.dtype == bool else np.full(len(idx), np.nan)
        out[inside] = v[idx[inside]]
        return out

    def audio_ms_to_elapsed(self, audio_ms, clock=None) -> float:
        c = clock or self.clock
        return float(c.sample_time_ns(audio_ms / 1000.0 * self.rate) / 1e6)


# ── Mouth ────────────────────────────────────────────────────────────────────


def _mouth_from_shapes(get) -> np.ndarray:
    """Mouth opening: jaw opening plus the lower lip's drop (when present)."""
    jaw = np.asarray(get("jawOpen"), dtype=np.float64)
    try:
        left = np.asarray(get("mouthLowerDownLeft"), dtype=np.float64)
        right = np.asarray(get("mouthLowerDownRight"), dtype=np.float64)
    except KeyError:
        return jaw
    return jaw + (left + right) / 2.0


def load_mouth(session: Path, video: Path, cam: int, min_conf: float, t_start: float, total_steps):
    """Per-frame elapsed times (ms) and mouth opening (``nan`` without a face).

    Uses the Face Dynamics CSV when it exists (no second pass over the
    video), else measures the face here.
    """
    import cv2
    import run_face_dynamics as rfd

    fd_csv = session / "face_dynamics" / f"{video.stem}.face_dynamics.csv"
    if fd_csv.exists():
        with fd_csv.open(newline="") as f:
            rows = list(csv.DictReader(f))
        if rows and "jawOpen" in rows[0]:
            times = np.array([float(r["timestamp_ms"]) for r in rows])
            seen = np.array([r["face_detected"] == "1" for r in rows])

            def get(name):
                return [float(r[name]) if r[name] != "" else np.nan for r in rows]

            mouth = _mouth_from_shapes(get)
            mouth[~seen] = np.nan
            log(f"Mouth movement from {fd_csv.name} (Face Dynamics).")
            return times, mouth, "face_dynamics"

    from face_dynamics.extract import FaceTracker

    cap = cv2.VideoCapture(str(video))
    if not cap.isOpened():
        raise RuntimeError(f"cannot open {video}")
    fps = cap.get(cv2.CAP_PROP_FPS) or 25.0
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    k, d, _ = rfd.camera_intrinsics(
        session,
        cam,
        int(cap.get(cv2.CAP_PROP_FRAME_WIDTH)),
        int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT)),
    )
    times = rfd.frame_times_ms(rfd.read_timestamps_ms(video, cam), total, fps)
    tracker = FaceTracker(k, d, min_confidence=min_conf)
    every = max(1, total // 100)
    try:
        m = rfd.measure_video(
            cap,
            total,
            tracker,
            lambda i: progress_line(i, total_steps(total), t_start) if i % every == 0 else None,
        )
    finally:
        tracker.close()
        cap.release()
    if "jawOpen" in m.shapes:
        mouth = _mouth_from_shapes(lambda name: m.shapes[name])
    else:  # no face in any frame
        mouth = np.full(m.n, np.nan)
    mouth[~m.found] = np.nan
    log("Mouth movement measured from the video (run Face Dynamics first to skip this).")
    return times[: m.n], mouth, "measured"


# ── Analysis ─────────────────────────────────────────────────────────────────


def analyse_camera(audio: Audio, frame_ms: np.ndarray, mouth: np.ndarray) -> dict:
    from conversation import speakers as spk
    from conversation import timing as tm
    from conversation import turns as tt

    clock = audio.clock
    hop_ms = audio.hop_time_ms(clock)
    start = max(frame_ms[0], hop_ms[0])
    end = min(frame_ms[-1], hop_ms[-1])
    if end - start < 5000:
        raise RuntimeError("the audio and this video overlap by less than 5 s")
    grid_ms = np.arange(start, end, HOP_S * 1000.0)
    g = (grid_ms - start) / 1000.0
    mouth_g = spk.mouth_on_grid((frame_ms - start) / 1000.0, mouth, g)
    activity = spk.mouth_activity(mouth_g, HOP_S)

    # Lip sync is only measurable while the person on camera talks: needs
    # speech with the face in view.
    speech0 = audio.on_grid(audio.speech, grid_ms, clock)
    talk_s = float((speech0 & np.isfinite(activity)).sum() * HOP_S)
    if talk_s >= 10.0:
        av_lag_s, av_r, prom = tm.estimate_av_lag(
            activity, audio.on_grid(audio.db, grid_ms, clock), HOP_S
        )
        reliable = tm.lag_is_reliable(av_r, prom)
    else:
        av_lag_s, av_r, reliable = 0.0, 0.0, False
    timing = {
        "method": clock.method,
        "av_lag_ms": round(av_lag_s * 1000.0) if reliable else None,
        "av_correlation": round(av_r, 3),
    }
    if clock.method == "session_start":
        if reliable:
            clock = clock.shifted(-av_lag_s, "session_start+av_lag")
            timing["method"] = clock.method
            log(
                f"Audio placed from the session start, corrected by {-av_lag_s * 1000:.0f} ms "
                "from lip movement."
            )
        else:
            log(
                "Audio placed from the session start (no timing file, and lip movement did not "
                "pin it down): expect up to a few hundred ms of offset between sound and picture."
            )
    elif reliable and abs(av_lag_s) > 0.15:
        log(
            f"Note: lip movement suggests the audio is {av_lag_s * 1000:.0f} ms off "
            "its timing file."
        )
    timing["audio_start_ms"] = round(float(clock.sample_time_ns(0)) / 1e6, 1)
    timing["clock_step_ms"] = round(clock.step_ms, 1)
    if clock.knots_k is not None:
        log(
            f"The audio timing file shows the clocks stepping apart by up to "
            f"{clock.step_ms:.0f} ms "
            "(audio lost while the program stalled); corrected piecewise."
        )
    timing["clock_ppm"] = round((clock.ns_per_sample * audio.rate / 1e9 - 1.0) * 1e6, 1)

    speech = audio.on_grid(audio.speech, grid_ms, clock)
    db_g = audio.on_grid(audio.db, grid_ms, clock)
    seen = np.isfinite(activity)
    thr = spk.speaking_threshold(activity, speech, HOP_S)
    if thr is None:
        vis = strong = np.zeros(len(g), bool)
        log("Too little silence with the face in view to calibrate mouth movement.")
    else:
        vis, strong = spk.visual_speaking(activity, thr, HOP_S)

    subject_label = None
    subject_cov = other_cov = None
    scores: dict = {}
    if audio.diarized:
        segs = [
            {
                "start_ms": audio.audio_ms_to_elapsed(s["start_ms"], clock) - start,
                "end_ms": audio.audio_ms_to_elapsed(s["end_ms"], clock) - start,
                "speaker": s.get("speaker"),
            }
            for s in audio.segments
        ]
        labels = spk.labels_on_grid(segs, g)
        subject_label, scores = spk.match_diarized_speaker(labels, speech, vis, seen)
        if subject_label is not None:
            names = {s["speaker"] for s in segs if s.get("speaker")}
            subject_cov = spk.label_coverage(segs, g, {subject_label})
            other_cov = spk.label_coverage(segs, g, names - {subject_label})
        else:
            log(
                "No diarized speaker clearly matches the face's mouth movement; "
                "using mouth movement alone."
            )
    who = spk.attribute(speech, seen, vis, strong, subject_cov, other_cov, HOP_S)
    method = "diarization" if subject_label is not None else "mouth"

    subj_iv = tt.merge_close(tt.mask_intervals(who["subject"], HOP_S), tt.MIN_PAUSE_S)
    other_iv = tt.merge_close(tt.mask_intervals(who["other"], HOP_S), tt.MIN_PAUSE_S)
    spurts = tt.classify_spurts(subj_iv, other_iv)
    turns, transitions = tt.build_turns(spurts)
    overlaps = tt.overlap_intervals(who["subject"], who["other"], HOP_S)

    # Words: each transcript segment goes to whoever spoke most of it.
    words = {tt.SUBJECT: 0, tt.OTHER: 0}
    seg_roles = []
    for s in audio.segments:
        a = audio.audio_ms_to_elapsed(s["start_ms"], clock)
        b = audio.audio_ms_to_elapsed(s["end_ms"], clock)
        sel = (grid_ms >= a) & (grid_ms < b)
        if not sel.any():
            continue
        if subject_label is not None and s.get("speaker"):
            role = tt.SUBJECT if s["speaker"] == subject_label else tt.OTHER
        else:
            ns, no = who["subject"][sel].sum(), who["other"][sel].sum()
            if ns == no == 0:
                continue
            role = tt.SUBJECT if ns > no else tt.OTHER
        words[role] += len(s.get("text", "").split())
        seg_roles.append(((a + b) / 2.0 - start, role, s.get("text", "")))
    has_words = bool(audio.segments)
    summary = tt.summarise(spurts, turns, transitions, overlaps, words if has_words else None)
    summary["coverage"] = {
        "analysed_s": round(len(g) * HOP_S, 1),
        "face_seen_pct": round(100.0 * seen.mean(), 1),
        "unattributed_speech_s": round(float(who["unknown"].sum() * HOP_S), 1),
    }
    texts = []
    for t in turns:
        texts.append(
            " ".join(
                txt
                for mid, role, txt in seg_roles
                if role == t.speaker and t.start_s <= mid / 1000.0 <= t.end_s + 0.5
            )
        )
    return {
        "start_ms": float(start),
        "grid_s": g,
        "who": who,
        "db": db_g,
        "activity": activity,
        "spurts": spurts,
        "turns": turns,
        "turn_text": texts,
        "transitions": transitions,
        "overlaps": overlaps,
        "summary": summary,
        "timing": timing,
        "attribution": {
            "method": method,
            "subject_label": subject_label,
            "label_scores": scores,
            "speaking_threshold": None if thr is None else round(thr, 5),
        },
        "levels": {k: round(v, 1) for k, v in audio.levels.items()},
    }


# ── Outputs ──────────────────────────────────────────────────────────────────


def _ms(start_ms: float, s: float) -> int:
    return int(round(start_ms + s * 1000.0))


def _series(values, step: int) -> list:
    v = np.asarray(values, dtype=np.float64)
    n = len(v) // step
    if n == 0:
        return []
    import warnings

    with np.errstate(all="ignore"), warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)  # all-nan blocks: no face
        blocks = np.nanmean(v[: n * step].reshape(n, step), axis=1)
    return [None if not math.isfinite(x) else round(float(x), 4) for x in blocks]


def write_outputs(
    out_dir: Path,
    video: Path,
    audio: Audio,
    cam: int,
    video_start_ms: float,
    r: dict,
    settings: dict,
    video_rel,
):
    out_dir.mkdir(parents=True, exist_ok=True)
    s0 = r["start_ms"]
    step = int(round(SIGNAL_STEP_MS / (HOP_S * 1000.0)))
    doc = {
        "schema": SCHEMA,
        "source_video": video.name,
        "source_audio": audio.path.name,
        "camera_index": cam,
        "video_start_ms": round(video_start_ms, 1),
        "settings": settings,
        "annotated_video": video_rel,
        "timing": r["timing"],
        "attribution": r["attribution"],
        "speech_detector": audio.vad,
        "speech_levels_db": r["levels"],
        "summary": r["summary"],
        "turns": [
            {
                "speaker": t.speaker,
                "start_ms": _ms(s0, t.start_s),
                "end_ms": _ms(s0, t.end_s),
                "duration_ms": round(t.duration_s * 1000.0),
                "pauses": len(t.pauses),
                "text": txt,
            }
            for t, txt in zip(r["turns"], r["turn_text"], strict=True)
        ],
        "transitions": [
            {
                "from": x.from_speaker,
                "to": x.to_speaker,
                "prev_end_ms": _ms(s0, x.prev_end_s),
                "next_start_ms": _ms(s0, x.next_start_s),
                "fto_ms": round(x.fto_s * 1000.0),
                "interruption": x.interruption,
            }
            for x in r["transitions"]
        ],
        "spurts": [
            {
                "speaker": s.speaker,
                "start_ms": _ms(s0, s.start_s),
                "end_ms": _ms(s0, s.end_s),
                "kind": s.kind,
            }
            for s in r["spurts"]
        ],
        "overlaps": [[_ms(s0, a), _ms(s0, b)] for a, b in r["overlaps"]],
        "signals": {
            "start_ms": round(s0, 1),
            "step_ms": SIGNAL_STEP_MS,
            "audio_db": _series(r["db"], step),
            "mouth_activity": _series(r["activity"], step),
        },
    }
    path = out_dir / f"{video.stem}.conversation.json"
    path.write_text(json.dumps(doc, indent=1), encoding="utf-8")
    with (out_dir / f"{video.stem}.turns.csv").open("w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(
            ["speaker", "start_s", "end_s", "duration_s", "pauses", "start_ms", "end_ms", "text"]
        )
        for t in doc["turns"]:
            w.writerow(
                [
                    t["speaker"],
                    f"{(t['start_ms'] - video_start_ms) / 1000.0:.3f}",
                    f"{(t['end_ms'] - video_start_ms) / 1000.0:.3f}",
                    f"{t['duration_ms'] / 1000.0:.3f}",
                    t["pauses"],
                    t["start_ms"],
                    t["end_ms"],
                    t["text"],
                ]
            )
    with (out_dir / f"{video.stem}.transitions.csv").open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["from", "to", "at_s", "fto_ms", "interruption"])
        for x in doc["transitions"]:
            w.writerow(
                [
                    x["from"],
                    x["to"],
                    f"{(x['next_start_ms'] - video_start_ms) / 1000.0:.3f}",
                    x["fto_ms"],
                    int(x["interruption"]),
                ]
            )
    return path


# ── Annotated video ──────────────────────────────────────────────────────────

SUBJ_COLOR, OTHER_COLOR, BOTH_COLOR = (90, 200, 90), (230, 160, 60), (60, 60, 230)
WINDOW_S = 6.0  # timeline span shown, seconds either side of now


def render(video: Path, out_path: Path, frame_ms: np.ndarray, r: dict, progress) -> bool:
    import cv2
    from gaze.render import open_writer, partial_path

    cap = cv2.VideoCapture(str(video))
    fps = cap.get(cv2.CAP_PROP_FPS) or 25.0
    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    partial = partial_path(out_path)
    writer = open_writer(partial, fps, (w, h))
    if writer is None:
        cap.release()
        log("Cannot open a video writer; no annotated video.")
        return False
    scale = max(0.5, h / 720.0)
    font = cv2.FONT_HERSHEY_SIMPLEX
    s0 = r["start_ms"]
    g = r["grid_s"]
    subj, other = r["who"]["subject"], r["who"]["other"]
    turns, texts = r["turns"], r["turn_text"]
    transitions = r["transitions"]

    def text(img, s, org, color, size=0.7, thick=2):
        fg = max(1, int(round(thick * scale)))
        (tw, th), base = cv2.getTextSize(s, font, size * scale, fg)
        x, y = org
        # A darkened box behind the text: an outline drawn thicker than
        # the text is also wider (Hershey glyphs advance with thickness).
        pad = max(2, int(3 * scale))
        y0, y1 = max(0, y - th - pad), min(img.shape[0], y + base + pad)
        x0, x1 = max(0, x - pad), min(img.shape[1], x + tw + pad)
        box = img[y0:y1, x0:x1]
        box[:] = (box * 0.35).astype(img.dtype)
        cv2.putText(img, s, org, font, size * scale, color, fg, cv2.LINE_AA)

    done = False
    try:
        for i in range(len(frame_ms)):
            ok, img = cap.read()
            if not ok:
                break
            t = (frame_ms[i] - s0) / 1000.0
            k = int(round(t / HOP_S))
            inside = 0 <= k < len(g)
            a = inside and bool(subj[k])
            b = inside and bool(other[k])
            if a and b:
                label, color = "BOTH SPEAKING", BOTH_COLOR
            elif a:
                label, color = "ON CAMERA SPEAKING", SUBJ_COLOR
            elif b:
                label, color = "OTHER SPEAKING", OTHER_COLOR
            else:
                label, color = "", (0, 0, 0)
            if label:
                text(img, label, (int(14 * scale), int(36 * scale)), color, 0.9, 2)
            # The latest transition's offset, shown for 3 s.
            recent = [x for x in transitions if 0.0 <= t - x.next_start_s <= 3.0]
            if recent:
                x = recent[-1]
                kind = "interruption" if x.interruption else ("gap" if x.fto_s > 0 else "overlap")
                text(
                    img,
                    f"response {x.fto_s * 1000:+.0f} ms ({kind})",
                    (int(14 * scale), int(70 * scale)),
                    (235, 235, 235),
                    0.6,
                    1,
                )
            # Caption: the current turn's words.
            cur = [j for j, tu in enumerate(turns) if tu.start_s <= t <= tu.end_s]
            if cur and texts[cur[-1]]:
                words = texts[cur[-1]]
                if len(words) > 90:
                    words = "..." + words[-87:]
                text(img, words, (int(14 * scale), h - int(110 * scale)), (235, 235, 235), 0.55, 1)
            # Timeline: two rows (on camera, other) around now.
            x0, x1 = int(14 * scale), w - int(14 * scale)
            y0 = h - int(80 * scale)
            row = int(22 * scale)
            overlay = img[y0 - 4 : y0 + 2 * row + 8, x0:x1]
            overlay[:] = (overlay * 0.4).astype(img.dtype)
            lo = max(0, int((t - WINDOW_S) / HOP_S))
            hi = min(len(g), int((t + WINDOW_S) / HOP_S))
            span = x1 - x0
            for r_i, mask, c in ((0, subj, SUBJ_COLOR), (1, other, OTHER_COLOR)):
                seg = mask[lo:hi]
                if not seg.any():
                    continue
                edges = np.flatnonzero(np.diff(np.concatenate([[0], seg.astype(np.int8), [0]])))
                for e0, e1 in zip(edges[::2], edges[1::2], strict=True):
                    px0 = x0 + int(((lo + e0) * HOP_S - (t - WINDOW_S)) / (2 * WINDOW_S) * span)
                    px1 = x0 + int(((lo + e1) * HOP_S - (t - WINDOW_S)) / (2 * WINDOW_S) * span)
                    yy = y0 + r_i * row
                    cv2.rectangle(img, (px0, yy), (max(px0 + 1, px1), yy + row - 4), c, -1)
            for r_i, name in ((0, "on camera"), (1, "other")):
                cv2.putText(
                    img,
                    name,
                    (x0 + 4, y0 + r_i * row + row - 9),
                    font,
                    0.4 * scale,
                    (200, 200, 200),
                    1,
                    cv2.LINE_AA,
                )
            cx = x0 + span // 2
            cv2.line(img, (cx, y0 - 6), (cx, y0 + 2 * row + 6), (255, 255, 255), max(1, int(scale)))
            writer.write(img)
            progress(i + 1)
        else:
            done = True
    finally:
        writer.release()
        cap.release()
        if not done:
            partial.unlink(missing_ok=True)
    if not done:
        log("The video ended early while drawing; no annotated video.")
        return False
    partial.replace(out_path)
    return True


# ── Main ─────────────────────────────────────────────────────────────────────


def process_camera(session: Path, video: Path, cam: int, audio: Audio, args) -> bool:
    import cv2

    t_start = time.perf_counter()
    cap = cv2.VideoCapture(str(video))
    n_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    cap.release()
    draw = not args.no_video
    fd_csv = session / "face_dynamics" / f"{video.stem}.face_dynamics.csv"
    passes = (0 if fd_csv.exists() else 1) + (1 if draw else 0)
    total_steps = max(1, passes * n_frames)

    frame_ms, mouth, mouth_source = load_mouth(
        session, video, cam, args.min_confidence, t_start, lambda n: total_steps
    )
    if not np.isfinite(mouth).any():
        log(f"{video.name}: no face found; speech cannot be attributed to the person on camera.")
    r = analyse_camera(audio, frame_ms, mouth)

    out_dir = session / "conversation"
    out_video = out_dir / f"{video.stem}.conversation.mp4"
    video_rel = None
    if draw:
        out_dir.mkdir(parents=True, exist_ok=True)
        offset = total_steps - n_frames
        every = max(1, n_frames // 100)
        if render(
            video,
            out_video,
            frame_ms,
            r,
            lambda i: progress_line(offset + i, total_steps, t_start) if i % every == 0 else None,
        ):
            video_rel = f"{out_dir.name}/{out_video.name}"
    if video_rel is None and out_video.exists():
        out_video.unlink()

    settings = {
        "min_confidence": args.min_confidence,
        "diarization_used": r["attribution"]["method"] == "diarization",
        "mouth_source": mouth_source,
    }
    path = write_outputs(out_dir, video, audio, cam, float(frame_ms[0]), r, settings, video_rel)
    s = r["summary"]
    subj, oth = s["speakers"]["subject"], s["speakers"]["other"]
    fto = s["transitions"]["floor_transfer_offset"]["median_s"]
    log(
        f"Done in {time.perf_counter() - t_start:.1f}s: on camera {subj['speech_s']} s in "
        f"{subj['turns']} turns, other {oth['speech_s']} s in {oth['turns']} turns, "
        f"{s['transitions']['count']} transitions"
        + (f" (median offset {fto * 1000:.0f} ms)" if fto is not None else "")
        + f", attribution by {r['attribution']['method']}"
    )
    print(f"  100.0%  ({total_steps}/{total_steps})", flush=True)
    log(f"Results -> {path}")
    return True


def main(argv=None) -> int:
    import run_face_dynamics as rfd

    args = parse_args(argv)
    session = Path(args.session)
    try:
        meta = json.loads((session / "session_meta.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        meta = {}
    wavs = sorted((session / "audio").glob("*.wav"))
    if args.audio:
        wavs = [w for w in wavs if w.name == args.audio]
    if not wavs:
        print(f"{TAG} No audio to analyse in {session / 'audio'}", file=sys.stderr, flush=True)
        return 1
    videos = sorted((session / "video").glob("video_*.mp4"))
    if args.camera is not None:
        videos = [v for v in videos if rfd.camera_index_from_filename(v) == args.camera]
    if not videos:
        print(f"{TAG} No videos to analyse in {session / 'video'}", file=sys.stderr, flush=True)
        return 1

    log(f"Audio: {wavs[0].name}")
    if len(wavs) > 1 and not args.audio:
        log(
            f"{len(wavs)} microphones recorded; using the first. Choose another with "
            "--audio <file name>."
        )
    audio = Audio(wavs[0], meta, not args.no_diarization)
    log(
        f"Speech found in {100.0 * audio.speech.mean():.0f}% of {audio.duration_s:.0f} s of audio; "
        f"clock from {audio.clock.method.replace('_', ' ')}"
        + ("; diarized speaker labels available" if audio.diarized else "")
    )
    if audio.speech.mean() < 0.01:
        log(
            "Almost no speech found in the audio: is the right microphone selected, "
            "and was anyone talking?"
        )
    if not audio.segments:
        log(
            "No transcript: run Speaker Diarization first to get the words of each turn "
            "and speech rates."
        )

    failures = 0
    for pos, video in enumerate(videos, start=1):
        print(f"{TAG} Camera {pos}/{len(videos)}: {video.name}", flush=True)
        try:
            ok = process_camera(session, video, rfd.camera_index_from_filename(video), audio, args)
        except Exception:  # one camera's failure must not stop the others
            traceback.print_exc()
            log(f"{video.name}: failed (see the error above)")
            ok = False
        failures += not ok
    return 1 if failures == len(videos) else 0


if __name__ == "__main__":
    sys.exit(main())
