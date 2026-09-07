"""Spectrogram, pitch and intensity analysis for MOSAIC's Speaker
Diarization analysis plugin.

Only the pure-numpy :mod:`voice.spectro` half is re-exported here. The
parselmouth layer (:mod:`voice.praat`) is deliberately left out: importing it
loads the Praat extension module, and both ``analysis/tests/test_voice_spectro.py``
and ``run_voice.py``'s argument parsing must work on a checkout where those
bindings are absent. Import it explicitly — ``from voice.praat import ...`` —
at the point where Praat is actually needed.
"""

from .spectro import (
    DEFAULT_COLS_PER_SECOND,
    MAX_COLUMNS,
    MIN_COLUMNS,
    SPECTROGRAM_ROWS,
    WINDOW_LENGTH_S,
    accumulate_column_max,
    auto_pitch_range,
    column_indices,
    db_to_uint8,
    decimate_track,
    drop_short_voiced_runs,
    dynamic_range,
    fill_empty_columns,
    reduce_freq_mean,
    spectrogram_time_step,
    suppress_low_intensity_pitch,
    target_columns,
    track_step_ms,
)

__all__ = [
    "DEFAULT_COLS_PER_SECOND",
    "MIN_COLUMNS",
    "MAX_COLUMNS",
    "SPECTROGRAM_ROWS",
    "WINDOW_LENGTH_S",
    "target_columns",
    "spectrogram_time_step",
    "track_step_ms",
    "column_indices",
    "accumulate_column_max",
    "fill_empty_columns",
    "reduce_freq_mean",
    "dynamic_range",
    "db_to_uint8",
    "suppress_low_intensity_pitch",
    "drop_short_voiced_runs",
    "decimate_track",
    "auto_pitch_range",
]
