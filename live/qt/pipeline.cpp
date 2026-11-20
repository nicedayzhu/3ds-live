#include "pipeline.h"
namespace live {
Pipeline::Pipeline(Timeline &t, QString dir, int s, QString m, QString f, QString precision)
    : timeline(t), models(std::move(dir)), scale(s), mode(std::move(m)), motion(std::move(f)),
      rifePrecision(resolveRifePrecision(std::move(precision))) {
    timeline.setPictureMode(mode, motion != "off");
    preparing = mode != "off" && mode != "clean";
    decoder = std::thread(&Pipeline::decodeLoop, this);
    enhancer = std::thread(&Pipeline::enhanceLoop, this);
    interpolator = std::thread(&Pipeline::motionLoop, this);
}
Pipeline::~Pipeline() {
    {
        std::lock_guard lock(mutex);
        done = true;
    }
    decodeReady.notify_one();
    enhanceReady.notify_one();
    motionReady.notify_one();
    decoder.join();
    enhancer.join();
    interpolator.join();
}
void Pipeline::configure(QString m, QString f, int outputScale, QString precision) {
    std::lock_guard lock(mutex);
    precision = resolveRifePrecision(std::move(precision));
    const bool pictureChanged = mode != m || motion != f || scale != outputScale;
    if (pictureChanged) {
        scale = outputScale;
        mode = std::move(m);
        motion = std::move(f);
        ++generation;
        preparing = mode != "off" && mode != "clean";
        timeline.setPictureMode(mode, motion != "off");
        jobs.clear();
        error.clear();
    } else {
        if (precision == rifePrecision) return;
        if (motion == "rife") timeline.clearSynthetic();
    }
    const bool precisionChanged = precision != rifePrecision;
    rifePrecision = std::move(precision);
    if (precisionChanged) {
        rifeBackend.clear();
        rifeFallback.clear();
    }
    if (!pictureChanged && motion != "rife") return;
    ++motionGeneration;
    pendingMotion.reset();
    motionError.clear();
    motionReady.notify_one();
}
void Pipeline::clearSessionLocked(quint32 session) {
    ++sessionEpoch;
    ++generation;
    ++motionGeneration;
    latestWire = {};
    latestQuality = {};
    latestSource = {};
    latestSource.session = session;
    pending.clear();
    jobs.clear();
    pendingMotion.reset();
}
void Pipeline::resetSession(quint32 session) {
    std::lock_guard lock(mutex);
    clearSessionLocked(session);
    timeline.reset(session);
}
void Pipeline::submit(Pair pair, SourceFrame source) {
    std::lock_guard lock(mutex);
    if (source.session != latestSource.session) clearSessionLocked(source.session);
    bool updated = false;
    for (int i = 0; i < 2; ++i) {
        if (!source.panels[i] || pair.screens[i].isEmpty()) continue;
        if (latestSource.panels[i] && source.panels[i]->pts <= latestSource.panels[i]->pts) continue;
        latestSource.panels[i] = source.panels[i];
        latestWire[i] = std::move(pair.screens[i]);
        latestQuality[i] = pair.jpegQuality[i];
        if (i == 0) latestSource.arrival = source.arrival;
        updated = true;
    }
    if (!updated || !latestSource.panels[0]) return;
    source.panels = latestSource.panels;
    source.sequence = source.panels[0]->sequence;
    source.pts = source.panels[0]->pts;
    source.arrival = latestSource.arrival;
    pair.screens = latestWire;
    pair.jpegQuality = latestQuality;
    if (!pending.empty() && pending.back().source.pts == source.pts) {
        pending.back() = {std::move(pair), source, sessionEpoch};
        return;
    }
    if (pending.size() == 2) {
        timeline.diagnostics.add({"decode_overwrite", pending.front().source});
        pending.pop_front();
        ++skipped;
    }
    pending.push_back({std::move(pair), source, sessionEpoch});
    decodeReady.notify_one();
}
void Pipeline::decodeLoop() {
    Screens last;
    Images images;
    int epoch = -1;
    decltype(SourceFrame::panels) decodedPanels{};
    std::array<bool, 2> validWire{};
    std::array<int, 2> validQuality{};
    QImage original;
    while (true) {
        std::unique_lock lock(mutex);
        decodeReady.wait(lock, [&] { return done || !pending.empty(); });
        if (done)
            return;
        Packet packet = std::move(pending.front());
        pending.pop_front();
        const bool needOriginal = !usesFbcnn(mode);
        const int outputScale = scale.load();
        lock.unlock();
        const auto decodeStart = clockUs();
        if (epoch != packet.sessionEpoch) {
            epoch = packet.sessionEpoch;
            last = {};
            images = {};
            images[1] = QImage(320, 240, QImage::Format_RGB888);
            images[1].fill(Qt::black);
            decodedPanels = {};
            validWire = {};
            validQuality = {};
            original = {};
        }
        bool changed = false;
        const auto before = decodedPanels;
        for (int i = 0; i < 2; ++i) {
            if (!packet.source.panels[i]) continue;
            if (packet.pair.screens[i] != last[i]) {
                auto next = decode(packet.pair.screens[i], i ? 320 : 400);
                validWire[i] = !next.isNull();
                if (!validWire[i]) ++invalid;
                else if (next != images[i]) {
                    images[i] = std::move(next);
                    changed = true;
                }
                last[i] = packet.pair.screens[i];
            }
            if (validWire[i]) {
                decodedPanels[i] = packet.source.panels[i];
                validQuality[i] = packet.pair.jpegQuality[i];
            }
        }
        const bool topUpdated = decodedPanels[0] && (!before[0] || before[0]->pts != decodedPanels[0]->pts);
        const bool bottomUpdated = decodedPanels[1] && (!before[1] || before[1]->pts != decodedPanels[1]->pts);
        if (!decodedPanels[0] || (!topUpdated && !bottomUpdated)) continue;
        packet.source.panels = decodedPanels;
        packet.source.sequence = decodedPanels[0]->sequence;
        packet.source.pts = decodedPanels[0]->pts;
        decodeTotalUs += clockUs() - decodeStart;
        if (!needOriginal)
            original = {};
        else if (changed || original.width() != 400 * outputScale)
            original = compose(images, outputScale);
        lock.lock();
        if (sessionEpoch != packet.sessionEpoch) continue;
        timeline.put(packet.source, original, {}, false, packet.pair.fps);
        ++decoded;
        if (mode != "off" || motion != "off") {
            if (!jobs.empty() && jobs.back().source.pts == packet.source.pts)
                jobs.pop_back();
            if (jobs.size() == 4) {
                timeline.diagnostics.add({"enhance_overwrite", jobs.front().source});
                jobs.pop_front();
                ++overwritten;
            }
            jobs.push_back(Job{packet.source, images, generation, scale.load(), mode, motion, validQuality});
            enhanceReady.notify_one();
        }
    }
}
void Pipeline::enhanceLoop() {
    Imaging ai(models);
    Images last, output, previousOutput, nativeRestored, previousMotionInput;
    DeviceImages motionDevice, previousDevice;
    std::optional<Job> prev;
    int lastGen = -1, failedGen = -1;
    std::array<int, 2> previousQuality{};
    auto panelChanged = [&](const Job &job, int i) {
        return job.generation != lastGen || job.images[i].cacheKey() != last[i].cacheKey() ||
               (job.mode == "fbcnn-detail" && job.jpegQuality[i] != previousQuality[i]);
    };
    QImage restored;
    // Separate budgets for changing the upper screen, lower screen, or both in parallel.
    std::array<double, 4> restoreUs{0., 20000., 16000., 36000.};
    // Compile the two device-specific graphs on the inference thread while the sender connects.
    QString initialMode;
    {
        std::lock_guard lock(mutex);
        initialMode = mode;
    }
    if (usesFbcnn(initialMode) || initialMode == "gpu" || initialMode == "fast-detail" || initialMode == "sr") {
        try {
            Images blank{QImage(400, 240, QImage::Format_RGB888), QImage(320, 240, QImage::Format_RGB888)};
            for (auto &screen : blank)
                screen.fill(Qt::black);
            Images warm;
            ai.restorePair(blank, warm, initialMode);
        } catch (const std::exception &e) {
            std::lock_guard lock(mutex);
            if (generation == 0) {
                error = QString::fromUtf8(e.what());
                failedGen = 0;
            }
        }
    }
    while (true) {
        std::unique_lock lock(mutex);
        enhanceReady.wait(lock, [&] { return done || !jobs.empty(); });
        if (done)
            return;
        // Preserve consecutive source images when they can meet their display deadline.
        // The two screens often update at different rates, so account for exact cache hits.
        while (jobs.size() > 1) {
            const auto &next = jobs.front();
            // When a newer upper frame already contains this lower correction,
            // restore both changing panels together on the GPU.
            if (prev && next.source.session == prev->source.session &&
                next.source.pts == prev->source.pts && jobs[1].source.pts > next.source.pts) {
                jobs.pop_front();
                continue;
            }
            int mask = 0;
            for (int i = 0; i < 2; ++i)
                if (panelChanged(next, i))
                    mask |= 1 << i;
            double cost = 1000 + restoreUs[mask];
            const int reserve = usesFbcnn(next.mode) ? 18000 : 0;
            if (clockUs() + cost + reserve <= timeline.deadline(next.source))
                break;
            timeline.diagnostics.add({"enhance_overwrite", next.source});
            ++overwritten;
            jobs.pop_front();
        }
        Job current = std::move(jobs.front());
        jobs.pop_front();
        const bool shareDevice = current.mode == "fbcnn" && current.motion == "rife" && rifeBackend.startsWith("TensorRT");
        lock.unlock();
        if (current.generation == failedGen)
            continue;
        const qint64 start = clockUs();
        try {
            const bool hdRife = current.mode == "fbcnn-sr" && current.motion == "rife";
            bool topChanged = panelChanged(current, 0);
            bool bottomChanged = panelChanged(current, 1);
            cache += int(!topChanged) + int(!bottomChanged);
            if (!shareDevice) motionDevice = {};
            if (topChanged || bottomChanged) {
                auto begin = clockUs();
                Images changed{topChanged ? current.images[0] : QImage{},
                               bottomChanged ? current.images[1] : QImage{}};
                ai.restorePair(changed, output, current.mode, hdRife ? &nativeRestored : nullptr,
                               shareDevice ? &motionDevice : nullptr, current.jpegQuality);
                auto elapsed = clockUs() - begin;
                restoreTotalUs += elapsed;
                ++restoreCalls;
                auto &estimate = restoreUs[int(topChanged) + 2 * int(bottomChanged)];
                if (elapsed < 150000)
                    estimate = .75 * estimate + .25 * elapsed;
            }
            last = current.images;
            previousQuality = current.jpegQuality;
            if (current.mode != "off" && (topChanged || bottomChanged)) {
                auto begin = clockUs();
                restored = compose(output, current.scale);
                composeTotalUs += clockUs() - begin;
            }
            const Images &motionInput = hdRife ? nativeRestored :
                (current.mode == "sr" && current.motion == "rife" ? current.images : output);
            {
                std::lock_guard publish(mutex);
                if (generation != current.generation || current.source.session != latestSource.session)
                    continue;
                if (current.mode != "off") {
                    timeline.put(current.source, restored, current.mode);
                    ++enhanced;
                }
                preparing = false;
                inferUs = clockUs() - start;
                if (usesFbcnn(current.mode)) {
                    fbcnnBackend = ai.fbcnnBackend();
                    fbcnnFallback = ai.fbcnnFallback();
                }
                if (current.motion != "off" && prev && prev->source.session == current.source.session &&
                    prev->generation == current.generation && current.source.pts > prev->source.pts &&
                    current.source.pts - prev->source.pts <= 75000 && (topChanged || bottomChanged)) {
                    auto midpoint = current.source;
                    midpoint.pts = (current.source.pts + prev->source.pts) / 2;
                    if (pendingMotion)
                        ++motionReplaced;
                    pendingMotion = MotionJob{midpoint,
                                              previousMotionInput,
                                              motionInput,
                                              motionGeneration,
                                              current.scale,
                                              current.motion,
                                              usesFbcnn(current.mode) ? current.mode + "+" + current.motion
                                                                     : current.motion,
                                              hdRife ? previousOutput : Images{}, previousDevice, motionDevice};
                    motionReady.notify_one();
                }
            }
            lastGen = current.generation;
            if (current.motion != "off") {
                previousMotionInput = motionInput;
                previousDevice = motionDevice;
                if (hdRife) previousOutput = output;
            }
            prev = std::move(current);
        } catch (const std::exception &e) {
            if (!lock.owns_lock())
                lock.lock();
            if (generation == current.generation)
                error = QString::fromUtf8(e.what());
            failedGen = current.generation;
        }
    }
}
void Pipeline::motionLoop() {
    std::unique_ptr<Imaging> imaging;
    QString imagingPrecision;
    bool rifeWarm = false;
    int lastGeneration = -1, failedGeneration = -1;
    std::array<qint64, 2> estimates{};
    for (;;) {
        std::unique_lock lock(mutex);
        const auto needsWarmup = [&] {
            return motion == "rife" && rifeBackend.isEmpty() && failedGeneration != motionGeneration;
        };
        motionReady.wait(lock, [&] { return done || pendingMotion.has_value() || needsWarmup(); });
        if (done)
            return;
        if (needsWarmup()) {
            const int startingGeneration = motionGeneration;
            const QString requested = rifePrecision;
            lock.unlock();
            try {
                if (!imaging || imagingPrecision != requested) {
                    imaging.reset();
                    imaging = std::make_unique<Imaging>(models, requested);
                    imagingPrecision = requested;
                    rifeWarm = false;
                }
                if (!rifeWarm) {
                    imaging->warmRife();
                    rifeWarm = true;
                }
                lock.lock();
                if (motionGeneration == startingGeneration) {
                    rifeBackend = imaging->rifeBackend();
                    rifeFallback = imaging->rifeFallback();
                }
            } catch (const std::exception &e) {
                lock.lock();
                failedGeneration = startingGeneration;
                if (motionGeneration == startingGeneration) {
                    motionError = QString::fromUtf8(e.what());
                    if (imaging) rifeFallback = imaging->rifeFallback();
                }
            }
            continue;
        }
        auto job = std::move(*pendingMotion);
        pendingMotion.reset();
        if (job.generation == failedGeneration)
            continue;
        if (job.generation != lastGeneration)
            estimates = {};
        lastGeneration = job.generation;
        lock.unlock();
        const auto begin = clockUs();
        const auto budget = timeline.deadline(job.source) - begin;
        const bool fineFlow = job.method == "flow" && budget >= std::max<qint64>(8000, estimates[1] + 2000);
        auto &estimate = estimates[int(fineFlow)];
        if (estimate >= budget) {
            ++motionPredictedLate;
            continue;
        }
        try {
            if (!imaging)
                imaging = std::make_unique<Imaging>(models);
            Images halfway;
            bool rejected = false;
            if (job.method == "rife") {
                const auto priorInputs = imaging->rifeDeviceInputs();
                halfway = imaging->rifeMidpoints(job.before, job.after, nullptr, job.beforeGpu, job.afterGpu);
                const auto inputs = imaging->rifeDeviceInputs();
                if (inputs >= priorInputs) motionDeviceInputs += inputs - priorInputs;
            } else {
                for (int i = 0; i < 2; ++i) {
                    halfway[i] = imaging->midpoint(job.before[i], job.after[i], job.method, fineFlow, &rejected);
                    // A failed DIS prediction holds the pair on the same real frame.
                    if (rejected) break;
                }
            }
            if (rejected) {
                ++guardedPairs;
                const auto elapsed = clockUs() - begin;
                motionTimeUs = elapsed;
                estimate = (3 * estimate + elapsed) / 4;
                continue;
            }
            int generatedMask = 0;
            for (int i = 0; i < 2; ++i) {
                const auto key = halfway[i].cacheKey();
                if (key != job.before[i].cacheKey() && key != job.after[i].cacheKey())
                    generatedMask |= 1 << i;
            }
            if (!generatedMask) {
                ++heldPairs;
                continue;
            }
            if (!job.previousHd[0].isNull()) {
                for (int i = 0; i < 2; ++i)
                    halfway[i] = halfway[i].cacheKey() == job.before[i].cacheKey()
                        ? job.previousHd[i] : imaging->restore(halfway[i], "sr");
            }
            auto canvas = compose(halfway, job.scale);
            const auto elapsed = clockUs() - begin;
            motionTimeUs = elapsed;
            estimate = (3 * estimate + std::min<qint64>(elapsed, 60000)) / 4;
            lock.lock();
            if (job.generation == motionGeneration) {
                timeline.put(job.source, canvas, job.quality, true, 0, generatedMask);
                ++interpolated;
                if (job.method == "rife") {
                    rifeBackend = imaging->rifeBackend();
                    rifeFallback = imaging->rifeFallback();
                }
            }
        } catch (const std::exception &e) {
            if (!lock.owns_lock())
                lock.lock();
            failedGeneration = job.generation;
            if (motionGeneration == job.generation) {
                motionError = QString::fromUtf8(e.what());
                if (job.method == "rife" && imaging) {
                    rifeWarm = false;
                    rifeBackend = imaging->rifeBackend();
                    rifeFallback = imaging->rifeFallback();
                }
            }
        }
    }
}
QJsonObject Pipeline::stats() {
    std::lock_guard lock(mutex);
    return {{"decoded", decoded.load()},
            {"backend", mode == "fbcnn-sr"              ? (fbcnnBackend.isEmpty() ? QString{} : fbcnnBackend + " / SR DirectML")
                        : usesFbcnn(mode)               ? fbcnnBackend
                        : mode == "gpu" || mode == "fast-detail" || mode == "sr" ? QStringLiteral("DirectML")
                                                        : QStringLiteral("CPU")},
            {"backend_fallback", usesFbcnn(mode) ? fbcnnFallback : QString()},
            {"decode_overwrites", skipped.load()},
            {"decode_errors", invalid.load()},
            {"enhanced", enhanced.load()},
            {"enhancement_overwrites", overwritten.load()},
            {"screen_cache_hits", cache.load()},
            {"decode_total_us", decodeTotalUs.load()},
            {"restore_total_us", restoreTotalUs.load()},
            {"restored_pairs", restoreCalls.load()},
            {"enhanced_compose_total_us", composeTotalUs.load()},
            {"processing_ms", inferUs.load() / 1000.},
            {"interpolated", interpolated.load()},
            {"rife_precision", rifePrecision},
            {"motion_backend", motion == "rife" ? rifeBackend : motion == "flow" ? QStringLiteral("CPU DIS") : QString()},
            {"motion_backend_fallback", motion == "rife" ? rifeFallback : QString()},
            {"motion_deadline_skips", motionReplaced.load() + motionPredictedLate.load()},
            {"motion_pending_replaced", motionReplaced.load()},
            {"motion_predicted_late", motionPredictedLate.load()},
            {"motion_device_input_frames", motionDeviceInputs.load()},
            {"motion_quality_holds", guardedPairs.load()},
            {"motion_unchanged_holds", heldPairs.load()},
            {"motion_processing_ms", motionTimeUs.load() / 1000.},
            {"motion_error", motionError},
            {"model_preparing", preparing && error.isEmpty()},
            {"model_error", error}};
}
} // namespace live
