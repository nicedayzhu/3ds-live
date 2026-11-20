#pragma once
#include <QtGlobal>
#include <array>

namespace live {
// Sample local worker counters once per stats interval. A low receive rate or
// one expensive inference alone does not establish sustained processing overload.
class EffectLoad {
    struct Sample { qint64 calls = 0, skipped = 0; double frameBudgets = 0.; };
    std::array<Sample, 3> recent{};
    qint64 lastCalls = 0, lastCostUs = 0, lastSkipped = 0, lastTime = 0;
    int next = 0, healthy = 0;
    bool busy = false;
public:
    bool observe(qint64 now, qint64 calls, qint64 costUs, qint64 skipped, double sourceFps) {
        const auto attempts = calls - lastCalls, cost = costUs - lastCostUs, missed = skipped - lastSkipped;
        const auto elapsed = now - lastTime;
        const bool continuous = lastTime && elapsed >= 750000 && elapsed <= 2000000 &&
                                attempts >= 0 && cost >= 0 && missed >= 0 && sourceFps > 0;
        lastTime = now;
        lastCalls = calls;
        lastCostUs = costUs;
        lastSkipped = skipped;
        if (!continuous) {
            recent = {};
            healthy = 0;
            return busy = false;
        }
        recent[next] = {attempts, missed, cost * sourceFps / 1000000.};
        next = (next + 1) % int(recent.size());
        qint64 completed = 0, dropped = 0;
        double budgets = 0.;
        bool sustained = true;
        for (const auto &sample : recent) {
            sustained &= sample.skipped >= 2;
            completed += sample.calls;
            dropped += sample.skipped;
            budgets += sample.frameBudgets;
        }
        // A very slow GPU can finish just one inference across all three intervals.
        // Once confirmed, keep its advice while the worker is still backlogged.
        if (sustained && dropped * 5 >= dropped + completed &&
            (completed ? budgets >= completed : busy)) {
            healthy = 0;
            busy = true;
        } else if (busy) {
            if (++healthy >= 3) busy = false;
        }
        return busy;
    }
};
} // namespace live
