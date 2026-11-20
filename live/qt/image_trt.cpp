#include "image_trt.h"
#include <QtCore>
#include <NvInferRuntime.h>
#include <cuda_runtime.h>
#include <opencv2/core.hpp>
#include <algorithm>
#include <stdexcept>

namespace live {
// The producer finishes its CUDA stream before publishing the corresponding
// QImage. A motion job holds both endpoints until its own stream is collected.
struct DeviceEndpoint {
    float *pixels{};
    qint64 imageKey = 0;
    ~DeviceEndpoint() { if (pixels) cudaFree(pixels); }
};
namespace {
void checkCuda(cudaError_t status) {
    if (status != cudaSuccess)
        throw std::runtime_error(cudaGetErrorString(status));
}

struct Logger final : nvinfer1::ILogger {
    void log(Severity level, const char *message) noexcept override {
        if (level <= Severity::kERROR)
            qWarning("TensorRT: %s", message);
    }
};

static Logger &runtimeLogger() {
    // TensorRT uses the first logger registered in the process, even for a
    // later runtime on another worker. Both workers may outlive one another.
    static Logger logger;
    return logger;
}

class Rgb8Quantizer {
#if CUDART_VERSION >= 12080
    cudaLibrary_t library{};
    cudaKernel_t kernel{};
    bool tried = false;

    // OpenCV float->RGB888->RIFE planar float: cvRound(x * 255), clamp,
    // then multiply by float(1 / 255). PTX is compiled by the installed
    // driver and introduces no separate CUDA compiler or DLL in the bundle.
    static constexpr char code[] = R"ptx(
.version 6.0
.target sm_50
.address_size 64
.visible .entry rgb8normalize(.param .u64 source, .param .u64 dest, .param .u32 count) {
    .reg .pred %past;
    .reg .b32 %idx, %n, %block, %stride, %lane, %rounded;
    .reg .b64 %src, %dst, %byte;
    .reg .f32 %value;
    ld.param.u32 %n, [count];
    mov.u32 %block, %ctaid.x;
    mov.u32 %stride, %ntid.x;
    mov.u32 %lane, %tid.x;
    mad.lo.u32 %idx, %block, %stride, %lane;
    setp.ge.u32 %past, %idx, %n;
    @%past bra done;
    ld.param.u64 %src, [source];
    ld.param.u64 %dst, [dest];
    mul.wide.u32 %byte, %idx, 4;
    add.u64 %src, %src, %byte;
    add.u64 %dst, %dst, %byte;
    ld.global.f32 %value, [%src];
    mul.rn.f32 %value, %value, 0f437f0000;
    cvt.rni.s32.f32 %rounded, %value;
    max.s32 %rounded, %rounded, 0;
    min.s32 %rounded, %rounded, 255;
    cvt.rn.f32.s32 %value, %rounded;
    mul.rn.f32 %value, %value, 0f3b808081;
    st.global.f32 [%dst], %value;
done: ret;
})ptx";
#endif
  public:
    ~Rgb8Quantizer() {
#if CUDART_VERSION >= 12080
        if (library) cudaLibraryUnload(library);
#endif
    }
    bool available() {
#if CUDART_VERSION >= 12080
        if (!tried) {
            tried = true;
            if (qEnvironmentVariable("THREE_DS_LIVE_RIFE_DEVICE") != "0" &&
                cudaLibraryLoadData(&library, code, nullptr, nullptr, 0, nullptr, nullptr, 0) == cudaSuccess)
                cudaLibraryGetKernel(&kernel, library, "rgb8normalize");
        }
        return kernel != nullptr;
#else
        return false;
#endif
    }
    bool run(float *source, float *dst, unsigned int count, cudaStream_t stream) {
#if CUDART_VERSION >= 12080
        void *args[]{&source, &dst, &count};
        return cudaLaunchKernel(kernel, dim3((count + 255) / 256), dim3(256), args, 0, stream) == cudaSuccess;
#else
        return false;
#endif
    }
};

struct Screen {
    std::unique_ptr<nvinfer1::ICudaEngine> engine;
    std::unique_ptr<nvinfer1::IExecutionContext> context;
    cudaStream_t stream{};
    cudaGraphExec_t graph{};
    // The third tensor is FBCNN's quality estimate or RIFE's midpoint time.
    float *input{}, *restored{}, *scalar{}, *upload{}, *download{};
    int width = 0;
    size_t bytes = 0, inputBytes = 0;
    cv::Mat floatRgb;
    std::array<std::shared_ptr<DeviceEndpoint>, 5> endpointPool;

    ~Screen() {
        if (stream)
            cudaStreamSynchronize(stream);
        if (graph)
            cudaGraphExecDestroy(graph);
        context.reset();
        if (input)
            cudaFree(input);
        if (restored)
            cudaFree(restored);
        if (scalar)
            cudaFree(scalar);
        if (upload)
            cudaFreeHost(upload);
        if (download)
            cudaFreeHost(download);
        if (stream)
            cudaStreamDestroy(stream);
    }

    void prepare(nvinfer1::IRuntime &runtime, const QString &path, int w, ImageTrt::Model model) {
        const bool rife = model == ImageTrt::Rife;
        const char *inputName = rife ? "images" : "image";
        const char *outputName = rife ? "output" : "restored";
        const char *scalarName = rife ? "alpha" : "qf";
        width = w;
        bytes = size_t(w) * 240 * 3 * sizeof(float);
        inputBytes = rife ? 2 * bytes : bytes;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error(("Missing TensorRT engine: " + QFileInfo(path).fileName()).toStdString());
        auto plan = file.readAll();
        engine.reset(runtime.deserializeCudaEngine(plan.constData(), size_t(plan.size())));
        if (!engine || engine->getNbIOTensors() != 3)
            throw std::runtime_error("Incompatible TensorRT engine");
        context.reset(engine->createExecutionContext());
        const nvinfer1::Dims4 inputShape(1, rife ? 6 : 3, 240, w);
        if (!context || (!rife && !context->setInputShape(inputName, inputShape)))
            throw std::runtime_error("Unexpected TensorRT input shape");
        const auto validate = [&](const char *name, nvinfer1::Dims expected, bool isInput) {
            const auto dims = context->getTensorShape(name);
            const bool fits = dims.nbDims == expected.nbDims &&
                              std::equal(expected.d, expected.d + expected.nbDims, dims.d);
            if (!fits || engine->getTensorDataType(name) != nvinfer1::DataType::kFLOAT ||
                engine->getTensorIOMode(name) != (isInput ? nvinfer1::TensorIOMode::kINPUT : nvinfer1::TensorIOMode::kOUTPUT) ||
                engine->getTensorFormat(name) != nvinfer1::TensorFormat::kLINEAR ||
                engine->getTensorLocation(name) != nvinfer1::TensorLocation::kDEVICE)
                throw std::runtime_error("Unexpected TensorRT tensor layout");
        };
        validate(inputName, inputShape, true);
        validate(outputName, nvinfer1::Dims4(1, 3, 240, w), false);
        nvinfer1::Dims scalarShape{};
        scalarShape.nbDims = rife ? 1 : 2;
        scalarShape.d[0] = scalarShape.d[1] = 1;
        validate(scalarName, scalarShape, rife);
        checkCuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        checkCuda(cudaMalloc(&input, inputBytes));
        checkCuda(cudaMalloc(&restored, bytes));
        checkCuda(cudaMalloc(&scalar, sizeof(float)));
        if (rife) {
            const float midpoint = .5f;
            checkCuda(cudaMemcpy(scalar, &midpoint, sizeof(float), cudaMemcpyHostToDevice));
        }
        checkCuda(cudaMallocHost(&upload, inputBytes));
        checkCuda(cudaMallocHost(&download, bytes));
        if (!context->setTensorAddress(inputName, input) || !context->setTensorAddress(outputName, restored) ||
            !context->setTensorAddress(scalarName, scalar))
            throw std::runtime_error("TensorRT tensor binding failed");
        // Bindings, host buffers and tensor shapes are fixed for this screen.
        // Warm up outside capture so TensorRT can initialize its resources.
        memset(upload, 0, inputBytes);
        checkCuda(cudaMemcpyAsync(input, upload, inputBytes, cudaMemcpyHostToDevice, stream));
        if (!compute())
            throw std::runtime_error("TensorRT warmup failed");
        checkCuda(cudaStreamSynchronize(stream));
        if (cudaStreamBeginCapture(stream, cudaStreamCaptureModeThreadLocal) == cudaSuccess) {
            cudaGraph_t captured{};
            // FBCNN always has a host input; RIFE may instead use two device endpoints.
            const bool ok = (rife || cudaMemcpyAsync(input, upload, inputBytes, cudaMemcpyHostToDevice, stream) == cudaSuccess)
                            && compute();
            const auto ended = cudaStreamEndCapture(stream, &captured);
            if (ok && ended == cudaSuccess && captured) {
                cudaGraphExec_t executable{};
                if (cudaGraphInstantiate(&executable, captured, 0) == cudaSuccess)
                    graph = executable;
            }
            if (captured)
                cudaGraphDestroy(captured);
        }
    }

    bool compute() {
        return context->enqueueV3(stream) &&
               cudaMemcpyAsync(download, restored, bytes, cudaMemcpyDeviceToHost, stream) == cudaSuccess;
    }

    void pack(const QImage &im, float *dest) {
        if (im.width() != width || im.height() != 240 || im.format() != QImage::Format_RGB888)
            throw std::runtime_error("Unexpected TensorRT image dimensions or format");
        cv::Mat rgb(240, width, CV_8UC3, const_cast<uchar *>(im.constBits()), im.bytesPerLine());
        rgb.convertTo(floatRgb, CV_32F, 1. / 255);
        std::array<cv::Mat, 3> planes;
        for (int c = 0; c < 3; ++c)
            planes[c] = cv::Mat(240, width, CV_32F, dest + c * width * 240);
        cv::split(floatRgb, planes.data());
    }
    bool enqueue(const QImage &first, const QImage &second,
                 const std::shared_ptr<DeviceEndpoint> &firstGpu, const std::shared_ptr<DeviceEndpoint> &secondGpu) {
        const bool twoInputs = inputBytes != bytes;
        if (twoInputs == second.isNull())
            throw std::runtime_error("Invalid number of TensorRT input images");
        const bool resident = firstGpu && secondGpu && twoInputs &&
                              firstGpu->imageKey == first.cacheKey() && secondGpu->imageKey == second.cacheKey();
        if (resident) {
            checkCuda(cudaMemcpyAsync(input, firstGpu->pixels, bytes, cudaMemcpyDeviceToDevice, stream));
            checkCuda(cudaMemcpyAsync(input + bytes / sizeof(float), secondGpu->pixels, bytes, cudaMemcpyDeviceToDevice, stream));
        } else {
            pack(first, upload);
            if (twoInputs) pack(second, upload + bytes / sizeof(float));
            if (twoInputs || !graph)
                checkCuda(cudaMemcpyAsync(input, upload, inputBytes, cudaMemcpyHostToDevice, stream));
        }
        if (graph)
            checkCuda(cudaGraphLaunch(graph, stream));
        else if (!compute())
            throw std::runtime_error("TensorRT inference failed");
        return resident;
    }

    std::shared_ptr<DeviceEndpoint> retain(Rgb8Quantizer &quantizer) {
        if (!quantizer.available()) return {};
        for (auto &slot : endpointPool) {
            if (!slot) {
                auto frame = std::make_shared<DeviceEndpoint>();
                if (cudaMalloc(&frame->pixels, bytes) != cudaSuccess) return {};
                slot = std::move(frame);
            }
            // A slot stays immutable while retained by the previous frame,
            // the queued job, or interpolation currently executing.
            if (slot.use_count() == 1 && quantizer.run(restored, slot->pixels, unsigned(bytes / sizeof(float)), stream))
                return slot;
        }
        return {};
    }

    QImage collect() {
        checkCuda(cudaStreamSynchronize(stream));
        std::array<cv::Mat, 3> planes;
        for (int c = 0; c < 3; ++c)
            planes[c] = cv::Mat(240, width, CV_32F, download + c * width * 240);
        cv::merge(planes.data(), planes.size(), floatRgb);
        QImage result(width, 240, QImage::Format_RGB888);
        cv::Mat dst(240, width, CV_8UC3, result.bits(), result.bytesPerLine());
        floatRgb.convertTo(dst, CV_8U, 255);
        return result;
    }
};
} // namespace

struct ImageTrt::Impl {
    QLibrary library;
    std::unique_ptr<nvinfer1::IRuntime> runtime;
    Rgb8Quantizer quantizer;
    std::array<Screen, 2> screens;
    bool serialRife = false;
    quint64 deviceInputs = 0;
};
ImageTrt::ImageTrt(const QString &dir, Model model, const QString &precision) : impl(std::make_unique<Impl>()) {
    impl->serialRife = model == Rife && qEnvironmentVariableIntValue("THREE_DS_LIVE_RIFE_SERIAL") == 1;
    impl->library.setFileName(QDir(dir).absoluteFilePath("nvinfer_10.dll"));
    auto create = reinterpret_cast<decltype(&createInferRuntime_INTERNAL)>(
        impl->library.resolve("createInferRuntime_INTERNAL"));
    if (!create)
        throw std::runtime_error("TensorRT 10 runtime unavailable");
    impl->runtime.reset(static_cast<nvinfer1::IRuntime *>(create(&runtimeLogger(), NV_TENSORRT_VERSION)));
    if (!impl->runtime)
        throw std::runtime_error("Incompatible TensorRT runtime");
    const QString pattern = model == Rife ? "rife-" + precision + "-%1.plan" : "fbcnn-fp16-%1-l3.plan";
    for (int i = 0; i < 2; ++i) {
        int w = i ? 320 : 400;
        impl->screens[i].prepare(*impl->runtime, QDir(dir).filePath(pattern.arg(w)), w, model);
    }
}
ImageTrt::~ImageTrt() = default;
quint64 ImageTrt::deviceInputs() const { return impl->deviceInputs; }
int Imaging::deviceRgbMismatches(const std::shared_ptr<DeviceEndpoint> &frame, const QImage &im) {
    if (!frame || frame->imageKey != im.cacheKey()) return -1;
    const int count = im.width() * im.height();
    std::vector<float> data(size_t(count) * 3);
    checkCuda(cudaMemcpy(data.data(), frame->pixels, data.size() * sizeof(float), cudaMemcpyDeviceToHost));
    cv::Mat expected;
    cv::Mat(im.height(), im.width(), CV_8UC3, const_cast<uchar *>(im.constBits()), im.bytesPerLine())
        .convertTo(expected, CV_32F, 1. / 255);
    int mismatches = 0;
    for (int c = 0; c < 3; ++c) {
        cv::Mat plane;
        cv::extractChannel(expected, plane, c);
        const cv::Mat actual(im.height(), im.width(), CV_32F, data.data() + count * c);
        mismatches += cv::countNonZero(plane != actual);
    }
    return mismatches;
}
void ImageTrt::run(const Images &first, Images &output, const Images &second,
                   const DeviceImages &firstGpu, const DeviceImages &secondGpu, DeviceImages *deviceOutput) {
    for (int i = 0; i < 2; ++i) {
        if (!first[i].isNull()) {
            if (impl->screens[i].enqueue(first[i], second[i], firstGpu[i], secondGpu[i]))
                impl->deviceInputs += 2;
            if (deviceOutput) (*deviceOutput)[i] = impl->screens[i].retain(impl->quantizer);
            if (impl->serialRife) output[i] = impl->screens[i].collect();
        }
    }
    if (impl->serialRife) return;
    for (int i = 0; i < 2; ++i)
        if (!first[i].isNull()) {
            output[i] = impl->screens[i].collect();
            if (deviceOutput && (*deviceOutput)[i])
                (*deviceOutput)[i]->imageKey = output[i].cacheKey();
        }
}
} // namespace live
