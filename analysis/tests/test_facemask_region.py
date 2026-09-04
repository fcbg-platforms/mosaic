"""
Pure-logic tests for the whole-body masking primitives in facemask/masking.py
— no video, no model weights, no ultralytics import needed.

Kept separate from test_facemask.py so that file stays focused on the
face/box path it already covers, matching how the pose3d and expression
suites are split by concern.

The invariant most of these exist to protect is one-directional: every helper
here may only ever *add* coverage. Under-masking is a privacy leak; the
over-masking direction is merely ugly.
"""

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))

import cv2  # noqa: E402
from facemask.masking import (  # noqa: E402
    apply_mask_region,
    dilate_mask,
    rasterize_boxes,
    region_blur_kernel,
)


def _frame(h=40, w=60, value=128):
    return np.full((h, w, 3), value, dtype=np.uint8)


# ── dilate_mask ────────────────────────────────────────────────────────────


def test_dilate_expands_a_single_pixel_by_exactly_the_radius():
    mask = np.zeros((21, 21), dtype=bool)
    mask[10, 10] = True

    out = dilate_mask(mask, 3)

    # Rectangular structuring element: a (2r+1) square centred on the pixel.
    assert out.sum() == 7 * 7
    assert out[10, 10]
    assert out[7, 7] and out[13, 13]
    assert not out[6, 10]


def test_dilate_with_zero_radius_is_identity():
    # An off-by-one here would silently halve the safety margin.
    mask = np.zeros((10, 10), dtype=bool)
    mask[4, 4] = True
    assert np.array_equal(dilate_mask(mask, 0), mask)
    assert np.array_equal(dilate_mask(mask, -5), mask)


def test_dilate_clips_at_the_frame_edge_without_wrapping():
    mask = np.zeros((10, 10), dtype=bool)
    mask[0, 0] = True

    out = dilate_mask(mask, 2)

    assert out[0, 0] and out[2, 2]
    assert not out[-1, -1]  # must not wrap around


def test_dilate_of_an_empty_mask_stays_empty():
    # Must never hallucinate coverage where the segmenter found nobody.
    mask = np.zeros((10, 10), dtype=bool)
    assert not dilate_mask(mask, 5).any()


def test_dilate_only_ever_adds_coverage():
    # The privacy-relevant invariant, over a spread of shapes and radii.
    rng = np.random.default_rng(1234)
    for radius in range(0, 8):
        mask = rng.random((25, 25)) > 0.9
        out = dilate_mask(mask, radius)
        assert np.all(out[mask]), f"dilation dropped coverage at r={radius}"


# ── rasterize_boxes ────────────────────────────────────────────────────────


def test_rasterize_adds_the_box_and_keeps_existing_coverage():
    mask = np.zeros((20, 20), dtype=bool)
    mask[0, 0] = True

    out = rasterize_boxes(mask, [(5.0, 5.0, 8.0, 9.0)])

    assert out[0, 0]  # pre-existing coverage survives
    assert out[5:9, 5:8].all()
    assert not out[10, 10]


def test_rasterize_rounds_outward_not_to_nearest():
    # Deliberately unlike apply_mask()'s int(round()): this union is the only
    # guard added over the segmentation mask, so it errs toward covering more.
    mask = np.zeros((20, 20), dtype=bool)

    out = rasterize_boxes(mask, [(5.6, 5.6, 8.1, 8.1)])

    assert out[5, 5], "top-left must floor, not round up"
    assert out[8, 8], "bottom-right must ceil, not round down"


def test_rasterize_skips_degenerate_boxes():
    mask = np.zeros((10, 10), dtype=bool)
    out = rasterize_boxes(mask, [(5.0, 5.0, 5.0, 5.0), (8.0, 8.0, 2.0, 2.0)])
    assert not out.any()


def test_rasterize_clips_a_box_overhanging_the_frame_edge():
    mask = np.zeros((10, 10), dtype=bool)
    out = rasterize_boxes(mask, [(7.0, 7.0, 99.0, 99.0)])
    assert out[-1, -1]
    assert out[7:, 7:].all()


# ── region_blur_kernel ─────────────────────────────────────────────────────


def test_kernel_is_odd_at_least_three_and_fits_the_frame():
    for w, h in [(4, 4), (64, 48), (1920, 1080), (1280, 720)]:
        k = region_blur_kernel(w, h)
        assert k % 2 == 1
        assert k >= 3
        assert k <= w and k <= h


def test_kernel_grows_with_frame_size():
    assert region_blur_kernel(1920, 1080) > region_blur_kernel(640, 480)


# ── apply_mask_region ──────────────────────────────────────────────────────


def test_solid_fill_touches_only_the_masked_pixels():
    frame = _frame()
    original = frame.copy()
    mask = np.zeros(frame.shape[:2], dtype=bool)
    mask[10:20, 10:20] = True

    apply_mask_region(frame, mask, "box")

    assert (frame[10:20, 10:20] == 20).all()
    # The other half of the test: nothing outside the mask moved at all.
    untouched = ~mask
    assert np.array_equal(frame[untouched], original[untouched])


def test_blur_leaves_unmasked_pixels_bit_identical():
    rng = np.random.default_rng(7)
    frame = rng.integers(0, 255, (40, 60, 3), dtype=np.uint8)
    original = frame.copy()
    mask = np.zeros(frame.shape[:2], dtype=bool)
    mask[15:25, 20:40] = True

    apply_mask_region(frame, mask, "blur")

    untouched = ~mask
    assert np.array_equal(frame[untouched], original[untouched])


def test_blur_actually_blurs_the_masked_region():
    frame = np.zeros((40, 60, 3), dtype=np.uint8)
    frame[15:25, 20:40] = 255  # a hard-edged bright block
    mask = np.zeros(frame.shape[:2], dtype=bool)
    mask[15:25, 20:40] = True

    apply_mask_region(frame, mask, "blur")

    # Blurring pulls in the dark surroundings, so pure 0/255 no longer holds.
    values = set(np.unique(frame[mask]).tolist())
    assert values - {0, 255}, "masked region was not actually blurred"


def test_empty_mask_leaves_the_frame_untouched():
    frame = _frame()
    original = frame.copy()
    mask = np.zeros(frame.shape[:2], dtype=bool)

    apply_mask_region(frame, mask, "blur")

    assert np.array_equal(frame, original)


def test_shape_mismatch_raises_rather_than_broadcasting():
    frame = _frame(40, 60)
    mask = np.ones((10, 10), dtype=bool)
    with pytest.raises(ValueError):
        apply_mask_region(frame, mask, "blur")


def test_tiny_frame_does_not_raise():
    frame = _frame(2, 2)
    mask = np.ones((2, 2), dtype=bool)
    apply_mask_region(frame, mask, "blur")  # falls back to solid fill
    assert (frame == 20).all()


def test_cropped_blur_matches_a_whole_frame_blur_exactly():
    # apply_mask_region() blurs only the mask's bounding box, padded by half
    # the kernel. That padding is what makes the crop equivalent to blurring
    # the entire frame rather than merely close to it — this test is what
    # catches a bounding box that is off by a pixel.
    rng = np.random.default_rng(99)
    frame = rng.integers(0, 255, (80, 100, 3), dtype=np.uint8)
    mask = np.zeros(frame.shape[:2], dtype=bool)
    mask[30:50, 40:70] = True

    cropped = frame.copy()
    apply_mask_region(cropped, mask, "blur")

    k = region_blur_kernel(frame.shape[1], frame.shape[0])
    naive = frame.copy()
    whole = cv2.GaussianBlur(frame, (k, k), 0)
    np.copyto(naive, whole, where=mask[:, :, None])

    assert np.array_equal(cropped[mask], naive[mask])


# ── person_class_index ─────────────────────────────────────────────────────


def test_person_class_index_finds_person_anywhere_in_the_map():
    from facemask.segmenters import person_class_index

    assert person_class_index({0: "person", 1: "bicycle"}) == 0
    # Not hardcoded to 0 — a custom --seg-model need not put person first.
    assert person_class_index({3: "car", 7: "person"}) == 7


def test_person_class_index_is_case_and_whitespace_insensitive():
    from facemask.segmenters import person_class_index

    assert person_class_index({2: " Person "}) == 2


def test_person_class_index_raises_when_there_is_no_person_class():
    # Silently masking the wrong class would write out every frame with the
    # people untouched, so this must fail loudly at construction.
    from facemask.segmenters import person_class_index

    with pytest.raises(ValueError):
        person_class_index({0: "cat", 1: "dog"})


# ── run_face_mask pure helpers ─────────────────────────────────────────────


def test_effective_skip_forces_whole_body_to_every_frame():
    # A reused silhouette misaligns as the subject moves, and unlike a face box
    # it has no padding to absorb that — so the knob is refused rather than
    # honoured.
    from run_face_mask import effective_skip

    assert effective_skip("body", 5) == 1
    assert effective_skip("body", 1) == 1


def test_effective_skip_leaves_face_runs_alone():
    from run_face_mask import effective_skip

    assert effective_skip("face", 5) == 5
    assert effective_skip("face", 1) == 1
    assert effective_skip("face", 0) == 1  # never returns a modulus of zero


def test_output_name_separates_region_and_backend():
    # Two runs covering materially different pixels must never overwrite each
    # other — holding a face-masked file believing it is body-masked is the
    # failure this naming exists to prevent.
    from run_face_mask import output_name

    face = output_name(Path("video_0.mp4"), "face", "opencv")
    body = output_name(Path("video_0.mp4"), "body", "opencv")
    other_backend = output_name(Path("video_0.mp4"), "face", "mediapipe")

    assert face == "video_0.face.opencv.mp4"
    assert len({face, body, other_backend}) == 3
    assert all(n.endswith(".mp4") for n in (face, body, other_backend))


def test_seg_dilate_radius_scales_with_frame_height_and_has_a_floor():
    from run_face_mask import seg_dilate_radius

    assert seg_dilate_radius(1080) > seg_dilate_radius(720)
    # Absolute, not proportional to the person: a tiny frame still gets a
    # usable margin rather than rounding to zero.
    assert seg_dilate_radius(100) >= 4
    assert seg_dilate_radius(0) >= 4


def test_partial_path_keeps_the_real_video_extension():
    # OpenCV picks its muxer from the filename suffix, so a "…mp4.partial"
    # name matches no muxer and cv2.VideoWriter refuses to open at all —
    # which breaks every run instead of protecting anything.
    from run_face_mask import partial_path_for

    out = Path("/tmp/anon/video_0.body.mediapipe.mp4")
    partial = partial_path_for(out)

    assert partial.suffix == ".mp4"
    assert partial != out
    assert "partial" in partial.stem
    assert partial.parent == out.parent


def test_partial_path_survives_a_multi_dot_stem():
    from run_face_mask import partial_path_for

    partial = partial_path_for(Path("video_0.face.opencv.mp4"))
    assert partial.suffix == ".mp4"
    assert partial.name == "video_0.face.opencv.partial.mp4"
