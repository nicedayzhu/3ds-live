#include "pipeline.h"
namespace live {
static bool needsPictureModel(const QString &mode) {
    return usesFbcnn(mode) || usesSuperResolution(mode) || mode == "gpu" || mode == "fast-detail";
}

Pipeline::Pipeline(Timeline &t, QString dir, int s, QString m, QString f, QString precision)
    : timeline(t), models(std::move(dir)), scale(s), mode(std::move(m)), motion(std::move(f)),
      rifePrecision(resolveRifePrecision(std::move(precision))) {
    // Keep decoded pictures visible until the first enhanced picture is ready.
    timeline.setPictureMode("off", motion != "off");
    preparing = needsPictureModel(mode);
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
    const bool motionChanged = motion != f;
    const bool pictureChanged = mode != m || motionChanged || scale != outputScale;
    if (pictureChanged) {
        if (mode != m) {
            preparing = needsPictureModel(m);
            strictPicture = false;
            error.clear();
            pictureBackend.clear();
            pictureFallback.clear();
        }
        scale = outputScale;
        mode = std::move(m);
        motion = std::move(f);
        ++generation;
        timeline.setPictureMode(strictPicture ? mode : QStringLiteral("off"), motion != "off");
        jobs.clear();
        enhanceReady.notify_one();
    } else {
        if (precision == rifePrecision) return;
        if (motion == "rife") timeline.clearSynthetic();
    }
    const bool precisionChanged = precision != rifePrecision;
    rifePrecision = std::move(precision);
    if (precisionChanged || motionChanged) {
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
    if (!updated || (!latestSource.panels[0] && !latestSource.panels[1])) return;
    source.panels = latestSource.panels;
    const auto &lead = latestSource.panels[0] ? latestSource.panels[0] : latestSource.panels[1];
    source.sequence = lead->sequence;
    source.pts = lead->pts;
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
        const bool needOriginal = !strictPicture;
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
        if ((!decodedPanels[0] && !decodedPanels[1]) || (!topUpdated && !bottomUpdated)) continue;
        packet.source.panels = decodedPanels;
        const auto &lead = decodedPanels[0] ? decodedPanels[0] : decodedPanels[1];
        packet.source.sequence = lead->sequence;
        packet.source.pts = lead->pts;
        decodeTotalUs += clockUs() - decodeStart;
        if (!needOriginal)
            original = {};
        else if (changed || original.width() != 400 * outputScale)
            original = compose(images, outputScale);
        lock.lock();
        if (sessionEpoch != packet.sessionEpoch) continue;
        timeline.put(packet.source, original, {}, false, packet.pair.fps);
        ++decoded;
        if ((mode != "off" || motion != "off") && error.isEmpty()) {
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
    std::unique_ptr<Imaging> ai;
    Images last, output, previousOutput, nativeRestored, previousMotionInput;
    DeviceImages motionDevice, previousDevice;
    std::optional<Job> prev;
    int lastGen = -1;
    std::array<int, 2> previousQuality{};
    auto panelChanged = [&](const Job &job, int i) {
        return job.generation != lastGen || job.images[i].cacheKey() != last[i].cacheKey() ||
               (job.mode == "fbcnn-detail" && job.jpegQuality[i] != previousQuality[i]);
    };
    QImage restored;
    // Separate budgets for changing the upper screen, lower screen, or both in parallel.
    std::array<double, 4> restoreUs{0., 20000., 16000., 36000.};
    while (true) {
        std::unique_lock lock(mutex);
        enhanceReady.wait(lock, [&] { return done || preparing || !jobs.empty(); });
        if (done)
            return;
        if (preparing) {
            const auto requested = mode;
            const int probeGeneration = generation;
            lock.unlock();
            try {
                if (!ai) ai = std::make_unique<Imaging>(models);
                Images blank{QImage(400, 240, QImage::Format_RGB888), QImage(320, 240, QImage::Format_RGB888)}, warm;
                for (auto &screen : blank) screen.fill(Qt::black);
                ai->restorePair(blank, warm, requested);
                lock.lock();
                if (generation == probeGeneration) {
                    preparing = false;
                    pictureBackend = ai->pictureBackend(requested);
                    pictureFallback = ai->pictureFallback(requested);
                }
            } catch (const std::exception &e) {
                const QString fallback = ai ? ai->pictureFallback(requested) : QString{};
                ai.reset();
                lock.lock();
                if (generation == probeGeneration) {
                    preparing = false;
                    pictureFallback = fallback;
                    error = QString::fromUtf8(e.what());
                    jobs.clear();
                }
            }
            continue;
        }
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
        const qint64 start = clockUs();
        try {
            const bool hdRife = current.mode == "fbcnn-sr" && current.motion == "rife";
            bool topChanged = panelChanged(current, 0);
            bool bottomChanged = panelChanged(current, 1);
            cache += int(!topChanged) + int(!bottomChanged);
            if (!shareDevice) motionDevice = {};
            qint64 restoreElapsed = 0;
            if (current.mode == "off") {
                output = current.images;
            } else if (topChanged || bottomChanged) {
                auto begin = clockUs();
                Images changed{topChanged ? current.images[0] : QImage{},
                               bottomChanged ? current.images[1] : QImage{}};
                if (!ai) ai = std::make_unique<Imaging>(models);
                ai->restorePair(changed, output, current.mode, hdRife ? &nativeRestored : nullptr,
                               shareDevice ? &motionDevice : nullptr, current.jpegQuality);
                restoreElapsed = clockUs() - begin;
                auto &estimate = restoreUs[int(topChanged) + 2 * int(bottomChanged)];
                if (restoreElapsed < 150000)
                    estimate = .75 * estimate + .25 * restoreElapsed;
            }
            last = current.images;
            previousQuality = current.jpegQuality;
            if (current.mode != "off" && (topChanged || bottomChanged)) {
                auto begin = clockUs();
                restored = compose(output, current.scale);
                composeTotalUs += clockUs() - begin;
            }
            const Images &motionInput = hdRife ? nativeRestored :
                (usesSuperResolution(current.mode) && current.motion == "rife" ? current.images : output);
            {
                std::lock_guard publish(mutex);
                if (generation != current.generation || current.source.session != latestSource.session)
                    continue;
                if (current.mode != "off") {
                    if (topChanged || bottomChanged) {
                        restoreTotalUs += restoreElapsed;
                        ++restoreCalls;
                    }
                    if (usesFbcnn(current.mode) && !strictPicture) {
                        strictPicture = true;
                        timeline.setPictureMode(current.mode, motion != "off", true);
                    }
                    timeline.put(current.source, restored, current.mode);
                    ++enhanced;
                }
                inferUs = clockUs() - start;
                if (ai && current.mode != "off") {
                    pictureBackend = ai->pictureBackend(current.mode);
                    pictureFallback = ai->pictureFallback(current.mode);
                }
                if (current.motion != "off" && motionError.isEmpty() && prev && prev->source.session == current.source.session &&
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
            const QString fallback = ai ? ai->pictureFallback(current.mode) : QString{};
            ai.reset();
            if (!lock.owns_lock())
                lock.lock();
            if (generation == current.generation) {
                error = QString::fromUtf8(e.what());
                pictureBackend.clear();
                pictureFallback = fallback;
                jobs.clear();
                if (strictPicture) {
                    strictPicture = false;
                    timeline.setPictureMode("off", motion != "off");
                }
            }
        }
    }
}
void Pipeline::motionLoop() {
    std::unique_ptr<Imaging> imaging;
    QString imagingPrecision;
    bool rifeWarm = false;
    int lastGeneration = -1;
    std::array<qint64, 2> estimates{};
    for (;;) {
        std::unique_lock lock(mutex);
        const auto needsWarmup = [&] {
            return motion == "rife" && rifeBackend.isEmpty() && motionError.isEmpty();
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
                if (motionGeneration == startingGeneration) {
                    motionError = QString::fromUtf8(e.what());
                    pendingMotion.reset();
                    if (imaging) rifeFallback = imaging->rifeFallback();
                }
                lock.unlock();
                imaging.reset();
                rifeWarm = false;
            }
            continue;
        }
        auto job = std::move(*pendingMotion);
        pendingMotion.reset();
        if (job.generation != lastGeneration)
            estimates = {};
        lastGeneration = job.generation;
        lock.unlock();
        const auto begin = clockUs();
        const auto budget = timeline.deadline(job.source) - begin;
        const bool fineFlow = job.method == "flow" && budget >= std::max<qint64>(8000, estimates[1] + 2000);
        auto &estimate = estimates[int(fineFlow)];
        if (estimate >= budget) {
            lock.lock();
            if (job.generation == motionGeneration) ++motionPredictedLate;
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
            estimate = (3 * estimate + std::min<qint64>(elapsed, 60000)) / 4;
            lock.lock();
            if (job.generation == motionGeneration) {
                timeline.put(job.source, canvas, job.quality, true, 0, generatedMask);
                ++interpolated;
                motionTimeUs = elapsed;
                motionWorkTotalUs += elapsed;
                if (job.method == "rife") {
                    rifeBackend = imaging->rifeBackend();
                    rifeFallback = imaging->rifeFallback();
                }
            }
        } catch (const std::exception &e) {
            if (!lock.owns_lock())
                lock.lock();
            if (motionGeneration == job.generation) {
                motionError = QString::fromUtf8(e.what());
                pendingMotion.reset();
                if (job.method == "rife" && imaging) {
                    rifeBackend.clear();
                    rifeFallback = imaging->rifeFallback();
                }
            }
            lock.unlock();
            imaging.reset();
            rifeWarm = false;
        }
    }
}
QJsonObject Pipeline::stats() {
    std::lock_guard lock(mutex);
    return {{"decoded", decoded.load()},
            {"backend", needsPictureModel(mode) ? pictureBackend : QStringLiteral("CPU")},
            {"backend_fallback", pictureFallback},
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
            {"motion_work_total_us", motionWorkTotalUs.load()},
            {"motion_error", motionError},
            {"motion_preparing", motion == "rife" && rifeBackend.isEmpty() && motionError.isEmpty()},
            {"model_preparing", preparing && error.isEmpty()},
            {"model_error", error}};
}
} // namespace live
