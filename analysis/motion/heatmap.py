"""
Trajectory density heatmaps and trajectory plots for mouse tracking data.
"""

from __future__ import annotations

from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np


def generate_heatmap(
    trajectories: Dict[int, List[Tuple[float, float]]],
    frame_size: Tuple[int, int],
    output_path: str,
    sigma: float = 20.0,
    cmap: str = "hot",
    title: str = "Position Density Heatmap",
    show_trails: bool = True,
    dpi: int = 150,
) -> None:
    """Render a Gaussian-smoothed position density map.

    Parameters
    ----------
    trajectories : dict of int to list of tuple of float
        ``{animal_id: [(cx, cy), ...]}`` in pixel coordinates.
    frame_size : tuple of int
        ``(width, height)`` of the original video frame.
    output_path : str
        Destination PNG/PDF path.
    sigma : float, default 20.0
        Gaussian blur radius (px) for density smoothing.
    cmap : str, default "hot"
        Matplotlib colormap name (e.g. ``"hot"``, ``"viridis"``, ``"plasma"``).
    title : str, default "Position Density Heatmap"
        Plot title.
    show_trails : bool, default True
        If ``True``, overlay per-animal trajectory lines.
    dpi : int, default 150
        Output image resolution.
    """
    import matplotlib.pyplot as plt
    from scipy.ndimage import gaussian_filter

    w, h = frame_size

    # Accumulate occupancy grid
    density = np.zeros((h, w), dtype=np.float32)
    for positions in trajectories.values():
        for cx, cy in positions:
            xi, yi = int(round(cx)), int(round(cy))
            if 0 <= xi < w and 0 <= yi < h:
                density[yi, xi] += 1.0

    density = gaussian_filter(density, sigma=sigma)
    if density.max() > 0:
        density /= density.max()

    fig, ax = plt.subplots(figsize=(10, 8))
    ax.set_facecolor("#0a0a0a")
    fig.patch.set_facecolor("#0a0a0a")

    im = ax.imshow(
        density,
        cmap=cmap,
        origin="upper",
        aspect="auto",
        interpolation="bilinear",
        extent=[0, w, h, 0],
    )
    cbar = plt.colorbar(im, ax=ax)
    cbar.set_label("Relative occupancy", color="white")
    cbar.ax.yaxis.set_tick_params(color="white")
    plt.setp(cbar.ax.yaxis.get_ticklabels(), color="white")

    if show_trails and trajectories:
        palette = plt.cm.Set1(np.linspace(0, 1, max(len(trajectories), 1)))
        for (aid, positions), color in zip(trajectories.items(), palette):
            if len(positions) > 1:
                pts = np.array(positions)
                ax.plot(
                    pts[:, 0], pts[:, 1],
                    "-", color=color, alpha=0.45, linewidth=0.6,
                    label=f"Animal {aid}",
                )
        if len(trajectories) <= 8:
            leg = ax.legend(loc="upper right", fontsize=8, framealpha=0.4)
            for text in leg.get_texts():
                text.set_color("white")

    ax.set_title(title, color="white", fontsize=13, pad=10)
    ax.set_xlabel("X (px)", color="white")
    ax.set_ylabel("Y (px)", color="white")
    ax.tick_params(colors="white")
    for spine in ax.spines.values():
        spine.set_edgecolor("#444444")

    fig.tight_layout()
    fig.savefig(output_path, dpi=dpi, bbox_inches="tight",
                facecolor=fig.get_facecolor())
    plt.close(fig)


def generate_trajectory_plot(
    trajectories: Dict[int, List[Tuple[float, float]]],
    frame_size: Tuple[int, int],
    output_path: str,
    title: str = "Trajectory Plot",
    dpi: int = 150,
) -> None:
    """Plain trajectory line plot, one colour per animal.

    Parameters
    ----------
    trajectories : dict of int to list of tuple of float
        ``{animal_id: [(cx, cy), ...]}`` in pixel coordinates.
    frame_size : tuple of int
        ``(width, height)`` of the original video frame.
    output_path : str
        Destination PNG/PDF path.
    title : str, default "Trajectory Plot"
        Plot title.
    dpi : int, default 150
        Output image resolution.

    Notes
    -----
    Each trajectory is colour-coded by time (start = transparent, end =
    opaque); start marker is a circle, end marker is a square.
    """
    import matplotlib.pyplot as plt
    import matplotlib.patches as mpatches

    w, h = frame_size

    fig, ax = plt.subplots(figsize=(10, 8))
    ax.set_facecolor("#0d0d1a")
    fig.patch.set_facecolor("#0d0d1a")
    ax.set_xlim(0, w)
    ax.set_ylim(h, 0)  # image coordinates

    palette = plt.cm.tab10(np.linspace(0, 1, max(len(trajectories), 1)))
    legend_patches = []

    for (aid, positions), color in zip(sorted(trajectories.items()), palette):
        if not positions:
            continue
        pts = np.array(positions)

        # Colour-coded by time (start = transparent, end = opaque)
        n = len(pts)
        for i in range(1, n):
            alpha = 0.2 + 0.8 * (i / n)
            ax.plot(pts[i-1:i+1, 0], pts[i-1:i+1, 1],
                    "-", color=color, alpha=alpha, linewidth=0.9)

        # Start / end markers
        ax.plot(pts[0, 0], pts[0, 1], "o", color=color, markersize=5, alpha=0.9)
        ax.plot(pts[-1, 0], pts[-1, 1], "s", color=color, markersize=5, alpha=0.9)

        legend_patches.append(
            mpatches.Patch(color=color, label=f"Animal {aid}  ({n} frames)"))

    if legend_patches:
        leg = ax.legend(handles=legend_patches, loc="upper right",
                        fontsize=8, framealpha=0.35)
        for text in leg.get_texts():
            text.set_color("white")

    ax.set_title(title, color="white", fontsize=13, pad=10)
    ax.set_xlabel("X (px)", color="white")
    ax.set_ylabel("Y (px)", color="white")
    ax.tick_params(colors="white")
    for spine in ax.spines.values():
        spine.set_edgecolor("#444444")

    ax.grid(color="#222240", linewidth=0.4)

    fig.tight_layout()
    fig.savefig(output_path, dpi=dpi, bbox_inches="tight",
                facecolor=fig.get_facecolor())
    plt.close(fig)


def generate_velocity_histogram(
    velocities: List[float],
    output_path: str,
    mm_per_px: float = 1.0,
    title: str = "Velocity Distribution",
    dpi: int = 150,
) -> None:
    """Histogram of per-frame velocities, with median/mean markers.

    Parameters
    ----------
    velocities : list of float
        Per-frame velocity samples; non-positive values are excluded.
    output_path : str
        Destination PNG/PDF path.
    mm_per_px : float, default 1.0
        Only used to pick the x-axis unit label (``"mm/s"`` if not
        ``1.0``, else ``"px/frame"``) — velocities themselves must
        already be pre-scaled by the caller.
    title : str, default "Velocity Distribution"
        Plot title.
    dpi : int, default 150
        Output image resolution.
    """
    import matplotlib.pyplot as plt

    unit = "mm/s" if mm_per_px != 1.0 else "px/frame"

    fig, ax = plt.subplots(figsize=(8, 5))
    ax.set_facecolor("#0d0d1a")
    fig.patch.set_facecolor("#0d0d1a")

    vals = np.array([v for v in velocities if v > 0], dtype=np.float32)
    if vals.size:
        ax.hist(vals, bins=60, color="#5588ff", edgecolor="#2244aa", alpha=0.85)
        ax.axvline(float(np.median(vals)), color="#ffcc44", linewidth=1.2,
                   label=f"Median: {np.median(vals):.1f} {unit}")
        ax.axvline(float(np.mean(vals)),   color="#ff6644", linewidth=1.2,
                   linestyle="--", label=f"Mean: {np.mean(vals):.1f} {unit}")
        leg = ax.legend(fontsize=9, framealpha=0.4)
        for text in leg.get_texts():
            text.set_color("white")

    ax.set_title(title, color="white", fontsize=13, pad=10)
    ax.set_xlabel(f"Velocity ({unit})", color="white")
    ax.set_ylabel("Frame count", color="white")
    ax.tick_params(colors="white")
    for spine in ax.spines.values():
        spine.set_edgecolor("#444444")

    fig.tight_layout()
    fig.savefig(output_path, dpi=dpi, bbox_inches="tight",
                facecolor=fig.get_facecolor())
    plt.close(fig)
