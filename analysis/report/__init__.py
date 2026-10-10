"""Session report for MOSAIC (run_session_report.py): everything the analysis
plugins found about one recording, in one HTML page and one summary row.

* :mod:`report.collect`: reads each plugin's output, tolerating any missing.
* :mod:`report.summary`: the flat per-session row for statistics.
* :mod:`report.render`: the self-contained HTML page and its SVG charts.
"""
