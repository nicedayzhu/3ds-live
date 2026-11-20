#include "native_detail.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>

namespace live {
namespace {
struct EdgeSamples {
    int peaks = 0;
    int centers = 0;
    int shoulders = 0;

    void add(int a, int b, int c, int d) {
        const int center = c - b;
        const int peak = std::abs(center);
        if (peak < 24 || peak < std::abs(b - a) || peak <= std::abs(d - c))
            return;
        const int sign = center > 0 ? 1 : -1;
        ++peaks;
        centers += peak;
        shoulders += std::max(0, (b - a) * sign) + std::max(0, (d - c) * sign);
    }
};

float detailGain(const cv::Mat &gray, int jpegQuality) {
    // At a sharp transition most of the intensity change occurs in one step.
    // On a blurred edge its neighbouring steps continue in the same direction.
    EdgeSamples edges;
    for (int y = 0; y < gray.rows; y += 4) {
        const auto row = gray.ptr<uchar>(y);
        for (int x = 1; x + 2 < gray.cols; ++x)
            edges.add(row[x-1], row[x], row[x+1], row[x+2]);
    }
    for (int y = 1; y + 2 < gray.rows; ++y) {
        const auto a = gray.ptr<uchar>(y-1), b = gray.ptr<uchar>(y);
        const auto c = gray.ptr<uchar>(y+1), d = gray.ptr<uchar>(y+2);
        for (int x = 0; x < gray.cols; x += 4)
            edges.add(a[x], b[x], c[x], d[x]);
    }
    if (!edges.centers)
        return .75f;
    const float spread = float(edges.shoulders) / edges.centers;
    // Keep the original gain in the ambiguous band and on sparse or flat screens.
    const float gain = .75f - .33f * std::clamp((.40f-spread)/.15f, 0.f, 1.f)
                            + .97f * std::clamp((spread-.62f)/.28f, 0.f, 1.f);
    const float confidence = std::clamp((edges.peaks - 24) / 96.f, 0.f, 1.f);
    const float adaptive = .75f + (gain - .75f) * confidence;
    if (jpegQuality <= 0 || jpegQuality >= 36)
        return adaptive;
    // At low Q, boosting narrow restored edges also boosts their ringing.
    // Once the neighbouring steps exceed 42% of the peak, keep the original
    // correction for soft content. Sparse screens have less edge evidence.
    const float compression = std::clamp((36 - jpegQuality) / 16.f, 0.f, 1.f);
    const float narrow = std::clamp((.42f - spread) / .08f, 0.f, 1.f);
    return adaptive * (1.f - compression * narrow * confidence);
}
} // namespace

QImage nativeDetail(const QImage &clean, int jpegQuality) {
    cv::Mat src(clean.height(), clean.width(), CV_8UC3, const_cast<uchar *>(clean.constBits()),
                clean.bytesPerLine());
    cv::Mat gray, smooth, low, high;
    cv::cvtColor(src, gray, cv::COLOR_RGB2GRAY);
    const float gain = detailGain(gray, jpegQuality);
    if (gain == 0.f)
        return clean;
    const int limit = std::clamp(qRound(10.f + (gain - .75f) * 9.2f), 7, 18);
    cv::GaussianBlur(gray, smooth, {3, 3}, .9);
    cv::erode(gray, low, {});
    cv::dilate(gray, high, {});
    QImage result = clean.copy();
    for (int y = 1; y < clean.height() - 1; ++y) {
        const auto lum = gray.ptr<uchar>(y), blur = smooth.ptr<uchar>(y);
        const auto lo = low.ptr<uchar>(y), hi = high.ptr<uchar>(y);
        auto pixel = result.scanLine(y) + 3;
        for (int x = 1; x < clean.width() - 1; ++x, pixel += 3) {
            const int contrast = std::clamp(hi[x] - lo[x] - 8, 0, 24);
            if (!contrast) continue;
            const int detail = qRound((lum[x] - blur[x]) * contrast * (gain / 24.f));
            const int delta = std::clamp(detail,
                std::max({-limit, lo[x] - lum[x], -int(std::min({pixel[0], pixel[1], pixel[2]}))}),
                std::min({limit, hi[x] - lum[x], 255 - std::max({pixel[0], pixel[1], pixel[2]})}));
            for (int c = 0; c < 3; ++c) pixel[c] += delta;
        }
    }
    return result;
}
} // namespace live
