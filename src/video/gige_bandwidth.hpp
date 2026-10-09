#pragma once
#include <QString>

namespace mosaic {

// What a camera configuration costs on the wire, and whether a gigabit link
// can carry it.
//
// Judged from the pixel format the camera *reports* after open(), never the
// one in the settings. The setting is only a request: VideoGrabber tries it,
// then a short fallback list, and a camera that accepts none of them simply
// keeps the format it was already in. At 1920x1080 and 25 fps the answer
// crosses the link's limit and back depending on that format: 155 MB/s in
// BGR8, 104 in YUV422, 52 in Bayer, against 125 for gigabit. A check run
// against the requested format can report an overload that does not exist, or
// miss one that does.
//
// QtCore-only (QString) so mosaic_tests can cover it: that binary links only
// Qt6::Core and Qt6::Network, the same constraint as camera_health.hpp.

/// Gigabit Ethernet line rate in bytes per second.
///
/// The nominal figure, not a derated one. Real GVSP throughput is lower
/// (packet headers, inter-packet gaps), so a configuration measuring at 100%
/// of this will not work. Anything approaching it is already in trouble, and
/// derating the constant would only hide by how much.
inline constexpr double k_gige_line_rate_bytes_per_sec = 125'000'000.0;

/// Above this share of the line rate a stream is reported as being at risk:
/// header overhead takes several percent, and a link run at its edge is where
/// a merely imperfect cable or connector starts losing packets.
inline constexpr double k_gige_link_warn_utilisation = 0.90;

/// Bytes per pixel on the wire for a GenICam pixel format name.
///
/// Covers the formats the camera card offers, the SFNC 1.x "Packed" spellings
/// VideoGrabber falls back to, and the other 8/10/12/16-bit Bayer, mono and
/// YUV formats in Pylon's GigE PixelFormat enum. Fractional for the packed
/// 10/12-bit and YUV 4:1:1 formats (1.5 bytes). Returns **0 for anything
/// unrecognised**, which callers read as "cannot judge": a made-up bandwidth
/// for an unknown format would look authoritative and be wrong.
///
/// BGR8 and RGB8 are three bytes because the *camera* interpolates colour
/// before sending; Bayer formats are the raw sensor mosaic, interpolated on
/// the host.
[[nodiscard]] double bytes_per_pixel(const QString& pixelFormat);

/// Whether two pixel format names mean the same format: equal, or the same
/// name with and without the SFNC 1.x "Packed" suffix ("BGR8" and
/// "BGR8Packed"). Used to tell a request the camera rejected from one it
/// accepted under its older spelling.
[[nodiscard]] bool same_pixel_format(const QString& a, const QString& b);

/// Bytes per second one camera puts on the wire. 0 when the format is unknown
/// or any input is not positive.
[[nodiscard]] double required_bytes_per_second(int width, int height, double fps,
                                               const QString& pixelFormat);

/// Share of a gigabit link this configuration needs: 1.0 is exactly the line
/// rate. 0.0 when the format is unknown.
[[nodiscard]] double gige_link_utilisation(int width, int height, double fps,
                                           const QString& pixelFormat);

/// The highest frame rate a gigabit link carries at this size and format. 0.0
/// when the format is unknown.
[[nodiscard]] double max_fps_for_link(int width, int height, const QString& pixelFormat);

} // namespace mosaic
