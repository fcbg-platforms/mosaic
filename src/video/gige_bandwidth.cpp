#include "video/gige_bandwidth.hpp"

#include <QHash>

namespace mosaic {

double bytes_per_pixel(const QString& pixelFormat) {
    // Exact GenICam enum names: they are case-sensitive, and both the plain
    // and the SFNC 1.x "Packed" spellings occur, depending on the firmware.
    static const QHash<QString, double> kTable{
        // Colour interpolated in the camera: three bytes per pixel.
        {"BGR8", 3.0},
        {"BGR8Packed", 3.0},
        {"RGB8", 3.0},
        {"RGB8Packed", 3.0},
        {"YUV444Packed", 3.0},
        // 4:2:2 chroma subsampling: four bytes per two pixels.
        {"YUV422Packed", 2.0},
        {"YUV422_YUYV_Packed", 2.0},
        {"YCbCr422_8", 2.0},
        // 4:1:1: six bytes per four pixels.
        {"YUV411Packed", 1.5},
        // Raw sensor mosaic or monochrome, 8 bits: one byte per pixel.
        {"Mono8", 1.0},
        {"BayerRG8", 1.0},
        {"BayerBG8", 1.0},
        {"BayerGR8", 1.0},
        {"BayerGB8", 1.0},
        // 10, 12 or 16 bits sent unpacked in two bytes...
        {"Mono10", 2.0},
        {"Mono12", 2.0},
        {"Mono16", 2.0},
        {"BayerRG10", 2.0},
        {"BayerBG10", 2.0},
        {"BayerGR10", 2.0},
        {"BayerGB10", 2.0},
        {"BayerRG12", 2.0},
        {"BayerBG12", 2.0},
        {"BayerGR12", 2.0},
        {"BayerGB12", 2.0},
        {"BayerRG16", 2.0},
        {"BayerBG16", 2.0},
        {"BayerGR16", 2.0},
        {"BayerGB16", 2.0},
        // ...or packed, two pixels in three bytes.
        {"Mono10Packed", 1.5},
        {"Mono12Packed", 1.5},
        {"BayerRG10Packed", 1.5},
        {"BayerBG10Packed", 1.5},
        {"BayerGR10Packed", 1.5},
        {"BayerGB10Packed", 1.5},
        {"BayerRG12Packed", 1.5},
        {"BayerBG12Packed", 1.5},
        {"BayerGR12Packed", 1.5},
        {"BayerGB12Packed", 1.5},
    };
    return kTable.value(pixelFormat, 0.0);
}

bool same_pixel_format(const QString& a, const QString& b) {
    static const QString kPacked = QStringLiteral("Packed");
    return a == b || a + kPacked == b || b + kPacked == a;
}

double required_bytes_per_second(int width, int height, double fps, const QString& pixelFormat) {
    const double bpp = bytes_per_pixel(pixelFormat);
    if (bpp <= 0.0 || width <= 0 || height <= 0 || fps <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(width) * height * bpp * fps;
}

double gige_link_utilisation(int width, int height, double fps, const QString& pixelFormat) {
    return required_bytes_per_second(width, height, fps, pixelFormat) /
           k_gige_line_rate_bytes_per_sec;
}

double max_fps_for_link(int width, int height, const QString& pixelFormat) {
    const double bpp = bytes_per_pixel(pixelFormat);
    if (bpp <= 0.0 || width <= 0 || height <= 0) {
        return 0.0;
    }
    return k_gige_line_rate_bytes_per_sec / (static_cast<double>(width) * height * bpp);
}

} // namespace mosaic
