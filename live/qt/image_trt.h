#pragma once
#include "imaging.h"

namespace live {
// One worker owns the two fixed-shape execution contexts and their streams.
class ImageTrt {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    enum Model { Fbcnn, Rife };
    ImageTrt(const QString &directory, Model model, const QString &precision);
    ~ImageTrt();
    // A null first image skips that screen. RIFE takes one additional endpoint.
    void run(const Images &first, Images &output, const Images &second = {},
             const DeviceImages &firstGpu = {}, const DeviceImages &secondGpu = {},
             DeviceImages *deviceOutput = nullptr);
    quint64 deviceInputs() const;
};
}
