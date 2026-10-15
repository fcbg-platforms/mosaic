"""Joining a person's track back after the tracker lost them for a moment."""

from __future__ import annotations

from pose.identity import stitch_tracks

SEAT = [240.0, 5.0, 860.0, 530.0]
PASSER = [0.0, 0.0, 1200.0, 530.0]


def _frames(spec, fps=40.0):
    """``spec``: per frame, a list of (id, box)."""
    return [
        {
            "frame_index": i,
            "timestamp_ns": int(1e12 + i * 1e9 / fps),
            "subjects": [{"subject_id": sid, "bbox_xyxy": list(box)} for sid, box in subs],
        }
        for i, subs in enumerate(spec)
    ]


def _ids(frames):
    return [[s["subject_id"] for s in f["subjects"]] for f in frames]


def test_someone_walking_past_does_not_rename_the_seated_person():
    # Seated person id 1 for 2.4 s; someone passes (id 5 grabs both for a
    # moment, then stays on the seated person); a short passer track 6.
    spec = [[(1, SEAT)]] * 96 + [[]] * 8 + [[(5, PASSER)]] * 6
    spec += [[(5, SEAT), (6, [500, 80, 1100, 530])]] * 4 + [[(5, SEAT)]] * 300
    frames = _frames(spec)
    assert stitch_tracks(frames) == {5: 1}
    ids = {i for f in _ids(frames) for i in f}
    assert ids == {1, 6}


def test_a_new_person_elsewhere_keeps_a_new_id():
    other_place = [900.0, 5.0, 1200.0, 530.0]
    frames = _frames([[(1, SEAT)]] * 80 + [[(2, other_place)]] * 80)
    assert stitch_tracks(frames) == {}


def test_too_long_an_absence_is_not_joined():
    frames = _frames([[(1, SEAT)]] * 40 + [[]] * 400 + [[(2, SEAT)]] * 40)  # 10 s gap
    assert stitch_tracks(frames) == {}


def test_two_people_at_once_are_never_merged():
    left, right = [0, 0, 400, 500], [380, 0, 800, 500]
    frames = _frames([[(1, left), (2, right)]] * 100)
    assert stitch_tracks(frames) == {}


def test_a_chain_of_pieces_goes_back_to_the_first_id():
    frames = _frames([[(1, SEAT)]] * 40 + [[(3, SEAT)]] * 40 + [[(7, SEAT)]] * 40)
    assert stitch_tracks(frames) == {3: 1, 7: 1}
    assert {i for f in _ids(frames) for i in f} == {1}


def test_untracked_detections_and_missing_timestamps():
    frames = _frames([[(1, SEAT)]] * 40 + [[(-1, SEAT)]] * 4 + [[(2, SEAT)]] * 40)
    for f in frames:
        f["timestamp_ns"] = 0  # no timestamps file: frame_index / fps instead
    assert stitch_tracks(frames, fps=40.0) == {2: 1}
    assert [-1] in _ids(frames)


def test_a_passer_starting_on_the_spot_does_not_take_the_hidden_persons_id():
    # The seated person (1) is lost; the passer's track (5) starts on the same
    # spot a moment before the person's own new track (7). The better match
    # wins, so the person stays 1 and the passer keeps a new id.
    passer = [150.0, 0.0, 950.0, 530.0]  # overlaps the seat (IoU about 0.6)
    spec = [[(1, SEAT)]] * 80 + [[(5, passer)]] * 4 + [[(5, passer), (7, SEAT)]] * 8
    spec += [[(7, SEAT)]] * 80
    frames = _frames(spec)
    assert stitch_tracks(frames) == {7: 1}
    assert [1, 5] in _ids(frames) or [5, 1] in _ids(frames)


def test_mixed_time_bases_use_frame_numbers():
    frames = _frames([[(1, SEAT)]] * 40 + [[(2, SEAT)]] * 40)
    frames[50]["timestamp_ns"] = 0  # one bad row in the timestamps file
    assert stitch_tracks(frames, fps=40.0) == {2: 1}
