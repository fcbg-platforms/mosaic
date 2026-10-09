"""Conversation timing for MOSAIC's Conversation Timing plugin
(run_conversation.py): who speaks when, turns, response times, pauses,
overlaps and backchannels, from a session's audio and one camera's view of a
face.

* :mod:`conversation.vad`: speech activity from the audio.
* :mod:`conversation.timing`: placing audio samples on the video clock.
* :mod:`conversation.speakers`: whether the face on camera is speaking, and
  which diarized speaker it is.
* :mod:`conversation.turns`: turns, transitions and the summary numbers.
"""
