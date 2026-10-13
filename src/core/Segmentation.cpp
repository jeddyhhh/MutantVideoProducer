#include "core/Segmentation.h"

#include "core/EffectHelpers.h"
#include "core/ImageOps.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <array>
#include <memory>
#include <mutex>

#ifdef MVP_ONNX
#include <onnxruntime_cxx_api.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dml_provider_factory.h>
#undef small
#endif
#endif

namespace {

// Steady's last mask, and Quality's memory of the frames before (its recurrent state).
struct MaskState : EffectState {
    QImage previous;
    QSize recurrentSize; // the input size the recurrent state belongs to
    std::array<std::vector<float>, 4> recurrent;
    std::array<std::vector<int64_t>, 4> recurrentShape;
};

MaskState& maskState(std::unique_ptr<EffectState>& state)
{
    if (!state)
        state = std::make_unique<MaskState>();
    return static_cast<MaskState&>(*state);
}

#ifdef MVP_ONNX
struct Model {
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> inputs, outputs;
    bool nchw = true;
    int w = 256, h = 256; // Fast: its fixed input size
    QString error;
    QString device = QStringLiteral("CPU");
    std::mutex run; // (DirectML runs one call at a time per session)
};

Model& model(SegmentModel which)
{
    static Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "mvp"};
    static Model models[2];
    static std::once_flag once[2];
    Model& m = models[int(which)];
    std::call_once(once[int(which)], [&] {
        const QString path = QDir(QCoreApplication::applicationDirPath() + QStringLiteral("/models"))
                                 .filePath(which == SegmentModel::Fast ? QStringLiteral("selfie_segmentation.onnx")
                                                                       : QStringLiteral("rvm_mobilenetv3_fp32.onnx"));
        if (!QFileInfo::exists(path)) {
            m.error = QStringLiteral("the segmentation model is missing (%1)").arg(path);
            return;
        }
        auto open = [&](bool gpu) {
            Ort::SessionOptions options;
            options.SetIntraOpNumThreads(2);
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
            if (gpu) { // DirectML: any DirectX 12 GPU
                options.DisableMemPattern();
                options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
                Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(options, 0));
            }
#endif
            m.session = std::make_unique<Ort::Session>(env, path.toStdWString().c_str(), options);
        };
        try {
            // The GPU unless there is none (or MVP_SEGMENT_CPU is set), else the CPU.
            bool onGpu = false;
#ifdef _WIN32
            if (qEnvironmentVariableIsEmpty("MVP_SEGMENT_CPU"))
                try {
                    open(true);
                    onGpu = true;
                } catch (const std::exception&) {
                    m.session.reset();
                }
#endif
            if (!onGpu)
                open(false);
            m.device = onGpu ? QStringLiteral("DirectML (GPU)") : QStringLiteral("CPU");
            Ort::AllocatorWithDefaultOptions alloc;
            for (size_t i = 0; i < m.session->GetInputCount(); ++i)
                m.inputs.push_back(m.session->GetInputNameAllocated(i, alloc).get());
            for (size_t i = 0; i < m.session->GetOutputCount(); ++i)
                m.outputs.push_back(m.session->GetOutputNameAllocated(i, alloc).get());
            const std::vector<int64_t> shape = m.session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
            if (which == SegmentModel::Fast && shape.size() == 4) { // NCHW (1, 3, h, w) or NHWC (1, h, w, 3)
                m.nchw = shape[1] == 3;
                const int64_t hh = m.nchw ? shape[2] : shape[1], ww = m.nchw ? shape[3] : shape[2];
                m.h = hh > 0 ? int(hh) : 256;
                m.w = ww > 0 ? int(ww) : 256;
            }
        } catch (const std::exception& e) {
            m.session.reset();
            m.error = QStringLiteral("the segmentation model could not be loaded: %1").arg(QString::fromLocal8Bit(e.what()));
        }
    });
    return m;
}

// The picture as a float tensor 0..1, NCHW (or NHWC).
std::vector<float> tensorOf(const QImage& picture, bool nchw)
{
    const QImage rgb = picture.convertToFormat(QImage::Format_RGB32);
    const int w = rgb.width(), h = rgb.height();
    const size_t plane = size_t(w) * h;
    std::vector<float> in(plane * 3);
    for (int y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const float c[3] = {qRed(row[x]) / 255.0f, qGreen(row[x]) / 255.0f, qBlue(row[x]) / 255.0f};
            const size_t i = size_t(y) * w + x;
            for (int k = 0; k < 3; ++k)
                in[nchw ? k * plane + i : i * 3 + k] = c[k];
        }
    }
    return in;
}

QImage maskOf(const float* v, int w, int h)
{
    QImage mask(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        uchar* row = mask.scanLine(y);
        for (int x = 0; x < w; ++x) {
            float p = v[size_t(y) * w + x];
            if (p < -0.01f || p > 1.01f) // logits
                p = 1.0f / (1.0f + std::exp(-p));
            row[x] = uchar(std::lround(std::clamp(p, 0.0f, 1.0f) * 255.0f));
        }
    }
    return mask;
}
#endif

} // namespace

bool segmentationAvailable(SegmentModel which, QString* error)
{
#ifdef MVP_ONNX
    if (!model(which).session && error)
        *error = model(which).error;
    return model(which).session != nullptr;
#else
    Q_UNUSED(which);
    if (error)
        *error = QStringLiteral("this build has no ONNX Runtime");
    return false;
#endif
}

QString segmentationDevice()
{
#ifdef MVP_ONNX
    return model(SegmentModel::Fast).session ? model(SegmentModel::Fast).device : QString();
#else
    return {};
#endif
}

QSize segmentationInputSize(SegmentModel which, QSize picture)
{
    if (which == SegmentModel::Quality) { // the picture's shape, at most 512 across, in multiples of 16
        const QSize s = picture.scaled(512, 512, Qt::KeepAspectRatio);
        return QSize(std::max(64, s.width() / 16 * 16), std::max(64, s.height() / 16 * 16));
    }
#ifdef MVP_ONNX
    return QSize(model(SegmentModel::Fast).w, model(SegmentModel::Fast).h);
#else
    return QSize(256, 256);
#endif
}

QImage personMask(const QImage& picture, QString* error)
{
    if (picture.isNull())
        return {};
    std::unique_ptr<EffectState> state;
    const QSize in = segmentationInputSize(SegmentModel::Fast, picture.size());
    const QImage mask = personMaskSmall(picture.scaled(in, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), SegmentModel::Fast, state, error);
    return mask.isNull() ? mask : mask.scaled(picture.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

void steadyMask(QImage& mask, std::unique_ptr<EffectState>& state)
{
    MaskState& s = maskState(state);
    if (s.previous.size() == mask.size())
        for (int y = 0; y < mask.height(); ++y) {
            uchar* row = mask.scanLine(y);
            const uchar* prev = s.previous.constScanLine(y);
            for (int x = 0; x < mask.width(); ++x)
                row[x] = uchar((row[x] * 3 + prev[x] * 2 + 2) / 5);
        }
    s.previous = mask;
}

QImage personMaskSmall(const QImage& picture, SegmentModel which, std::unique_ptr<EffectState>& state, QString* error)
{
#ifdef MVP_ONNX
    Model& m = model(which);
    if (!m.session || picture.isNull()) {
        if (error)
            *error = m.error;
        return {};
    }
    try {
        const Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<const char*> inNames, outNames;
        for (const std::string& n : m.inputs)
            inNames.push_back(n.c_str());
        for (const std::string& n : m.outputs)
            outNames.push_back(n.c_str());
        if (which == SegmentModel::Fast) {
            const QImage small = picture.size() == QSize(m.w, m.h) ? picture : picture.scaled(m.w, m.h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            std::vector<float> in = tensorOf(small, m.nchw);
            const std::array<int64_t, 4> shape = m.nchw ? std::array<int64_t, 4>{1, 3, m.h, m.w} : std::array<int64_t, 4>{1, m.h, m.w, 3};
            Ort::Value tensor = Ort::Value::CreateTensor<float>(mem, in.data(), in.size(), shape.data(), shape.size());
            std::unique_lock lock(m.run);
            std::vector<Ort::Value> out = m.session->Run(Ort::RunOptions{nullptr}, inNames.data(), &tensor, 1, outNames.data(), 1);
            lock.unlock();
            const size_t count = out[0].GetTensorTypeAndShapeInfo().GetElementCount(), plane = size_t(m.w) * m.h;
            if (count < plane)
                throw std::runtime_error("unexpected output size");
            const float* prob = out[0].GetTensorData<float>();
            return maskOf(count >= 2 * plane ? prob + plane : prob, m.w, m.h); // (two channels: the person's)
        }
        // Quality: RobustVideoMatting. Inputs src, r1i..r4i (its memory), downsample_ratio;
        // outputs fgr, pha (the matte), r1o..r4o. A new size starts its memory afresh.
        MaskState& s = maskState(state);
        const int w = picture.width(), h = picture.height();
        if (s.recurrentSize != picture.size()) {
            s.recurrentSize = picture.size();
            for (int k = 0; k < 4; ++k) {
                s.recurrent[size_t(k)] = {0.0f};
                s.recurrentShape[size_t(k)] = {1, 1, 1, 1};
            }
        }
        std::vector<float> src = tensorOf(picture, true);
        const std::array<int64_t, 4> srcShape{1, 3, h, w};
        float ratio = std::min(1.0f, 384.0f / float(std::max(w, h)));
        const std::array<int64_t, 1> ratioShape{1};
        std::vector<Ort::Value> inputs;
        for (const std::string& name : m.inputs) {
            if (name == "src") {
                inputs.push_back(Ort::Value::CreateTensor<float>(mem, src.data(), src.size(), srcShape.data(), srcShape.size()));
            } else if (name == "downsample_ratio") {
                inputs.push_back(Ort::Value::CreateTensor<float>(mem, &ratio, 1, ratioShape.data(), ratioShape.size()));
            } else { // r1i..r4i
                const size_t k = size_t(std::clamp(name[1] - '1', 0, 3));
                inputs.push_back(Ort::Value::CreateTensor<float>(mem, s.recurrent[k].data(), s.recurrent[k].size(), s.recurrentShape[k].data(),
                                                                 s.recurrentShape[k].size()));
            }
        }
        std::unique_lock lock(m.run);
        std::vector<Ort::Value> out = m.session->Run(Ort::RunOptions{nullptr}, inNames.data(), inputs.data(), inputs.size(), outNames.data(),
                                                     outNames.size());
        lock.unlock();
        QImage mask;
        for (size_t i = 0; i < m.outputs.size(); ++i) {
            const std::string& name = m.outputs[i];
            if (name == "pha") {
                mask = maskOf(out[i].GetTensorData<float>(), w, h);
            } else if (name.size() == 3 && name[0] == 'r' && name[2] == 'o') { // keep the memory for the next frame
                const size_t k = size_t(std::clamp(name[1] - '1', 0, 3));
                const auto info = out[i].GetTensorTypeAndShapeInfo();
                const float* v = out[i].GetTensorData<float>();
                s.recurrent[k].assign(v, v + info.GetElementCount());
                s.recurrentShape[k] = info.GetShape();
            }
        }
        return mask;
    } catch (const std::exception& e) {
        if (error)
            *error = QString::fromLocal8Bit(e.what());
        return {};
    }
#else
    Q_UNUSED(picture);
    Q_UNUSED(which);
    Q_UNUSED(state);
    if (error)
        *error = QStringLiteral("this build has no ONNX Runtime");
    return {};
#endif
}

void addSegmentationEffects(std::vector<EffectDef>& list)
{
    using namespace fx;
    EffectDef d;
    d.id = QStringLiteral("remove_bg");
    d.name = QStringLiteral("Remove Background (AI)");
    d.category = QStringLiteral("Keying & Compositing");
    d.description = QStringLiteral("Finds the people in the picture and removes, blurs or recolours everything else, "
                                   "no green screen needed (works on the Live tab too). Quality follows hair and fine edges, "
                                   "and keeps steady on video by itself.");
    ParamDef model = choice("model", "Model", {QStringLiteral("Fast"), QStringLiteral("Quality (video matting)")});
    model.tooltip = QStringLiteral("Fast: a quick person mask (Threshold and Edge softness shape its edge). "
                                   "Quality: a finer matte that follows hair, slower (used as it is)");
    d.params = {model,
                choice("mode", "Background", {QStringLiteral("Transparent"), QStringLiteral("Blurred"), QStringLiteral("Color"), QStringLiteral("Show mask")}),
                num("threshold", "Threshold", 0.05, 0.95, 0.5), num("softness", "Edge softness", 0, 0.8, 0.2),
                num("blur", "Background blur", 1, 80, 18, 0, " px"), color("color", "Background color", 0x00b140),
                flag("steady", "Steady (less flicker)", true), flag("invert", "Remove the person instead", false)};
    d.apply = [](QImage& img, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>& state) {
        // (The GPU renderer does the same steps: GpuCompositor::removeBackground.)
        const SegmentModel which = SegmentModel(std::clamp(p.choice(QStringLiteral("model")), 0, 1));
        QImage mask = personMaskSmall(img.scaled(segmentationInputSize(which, img.size()), Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
                                      which, state);
        if (mask.isNull())
            return; // no model: the picture as it is
        if (p.flag(QStringLiteral("steady")))
            steadyMask(mask, state);
        mask = mask.scaled(img.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const int mode = p.choice(QStringLiteral("mode"));
        const bool matte = which == SegmentModel::Quality;
        const float t = float(p.num(QStringLiteral("threshold"))), soft = float(p.num(QStringLiteral("softness"))) / 2;
        const bool invert = p.flag(QStringLiteral("invert"));
        QImage back;
        if (mode == 1) {
            back = img.copy();
            const double sigma = std::max(0.5, p.num(QStringLiteral("blur")) * ctx.scale / 2);
            imageops::gaussianBlur(back, sigma, sigma);
        }
        const QRgb color = p.color(QStringLiteral("color")).rgb();
        const int w = img.width();
        img.bits(); // detach before the threads
        parallelFor(img.height(), [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                auto* row = reinterpret_cast<uint32_t*>(img.scanLine(y));
                const uchar* m = mask.constScanLine(y);
                const auto* b = back.isNull() ? nullptr : reinterpret_cast<const uint32_t*>(back.constScanLine(y));
                for (int x = 0; x < w; ++x) {
                    float a = matte ? m[x] / 255.0f : imageops::smoothstep(t - soft, t + soft, m[x] / 255.0f);
                    if (invert)
                        a = 1.0f - a;
                    if (mode == 3) {
                        const int v = int(std::lround(a * 255));
                        row[x] = qRgb(v, v, v);
                        continue;
                    }
                    const uint32_t under = mode == 0 ? 0u : (mode == 1 ? b[x] : color);
                    const uint32_t k = uint32_t(a * 256.0f), s = 256 - k, pa = under, pb = row[x];
                    const uint32_t rb = (((pa & 0x00ff00ffu) * s + (pb & 0x00ff00ffu) * k) >> 8) & 0x00ff00ffu;
                    const uint32_t ag = ((((pa >> 8) & 0x00ff00ffu) * s + ((pb >> 8) & 0x00ff00ffu) * k)) & 0xff00ff00u;
                    row[x] = rb | ag;
                }
            }
        });
    };
    list.push_back(std::move(d));
}
