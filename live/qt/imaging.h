#pragma once
#include <QImage>
#include <QString>
#include <memory>
#include <array>
namespace live {
using Images = std::array<QImage, 2>;
struct DeviceEndpoint;
using DeviceImages = std::array<std::shared_ptr<DeviceEndpoint>, 2>;
inline bool usesFbcnn(const QString &mode) {
    return mode == "fbcnn" || mode == "fbcnn-detail" || mode == "fbcnn-sr";
}
inline bool usesSuperResolution(const QString &mode) { return mode == "sr" || mode == "fbcnn-sr" || mode == "span"; }
inline bool isNativeDetail(const QString &mode) { return mode == "fbcnn-detail" || mode == "fast-detail"; }
void configureOpenCV();
QString resolveRifePrecision(QString precision);
QImage decode(const QByteArray &wire, int width);
QImage compose(const Images &screens, int scale);
class Imaging {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    explicit Imaging(QString modelDirectory, QString rifePrecision = {});
    ~Imaging();
    QImage restore(const QImage &image, const QString &mode);
    // Null entries reuse the last screen result.
    void restorePair(const Images &changed, Images &output, const QString &mode, Images *nativeRestored = nullptr,
                     DeviceImages *deviceOutput = nullptr, const std::array<int, 2> &jpegQuality = {});
    QString pictureBackend(const QString &mode) const;
    QString pictureFallback(const QString &mode) const;
    QString rifeBackend() const;
    QString rifeFallback() const;
    void warmRife();
    // Null panels are skipped. A scene cut holds only the affected screen.
    Images rifeMidpoints(const Images &before, const Images &after, std::array<bool, 2> *cuts = nullptr,
                         const DeviceImages &beforeGpu = {}, const DeviceImages &afterGpu = {});
    quint64 rifeDeviceInputs() const;
    // Offline check: count normalized planar channels that differ from the
    // RGB888 image the regular host-input path would upload. -1 if unavailable.
    static int deviceRgbMismatches(const std::shared_ptr<DeviceEndpoint> &frame, const QImage &image);
    QImage midpoint(const QImage &a, const QImage &b, const QString &mode, bool fineFlow = true,
                    bool *unreliable = nullptr);
    // Generate interior samples, sharing one bidirectional motion estimate.
    QList<QImage> flowBetween(const QImage &a, const QImage &b, int divisions, bool fineFlow,
                              bool *unreliable = nullptr);
};
} // namespace live
