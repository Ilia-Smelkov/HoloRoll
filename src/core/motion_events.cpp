// v0.12.0-alpha.9: Motion event detection — implementations.
//
// First concrete detector: RigidMechanismDetector. Designed for "rigid"
// / mechanical animations (doors, levers, drawers, switches) where a
// bone moves purposefully between rest states. Detects 5 event types
// from the alpha.8 signed-projection signal:
//   start       — |motion| crosses enter_threshold from below.
//   peak_hi     — local maximum of motion[f] inside an active phase.
//   peak_lo     — local minimum of motion[f] inside an active phase.
//   zero_cross  — motion[f] crosses 0 (passes through trajectory mean).
//   end         — |motion| stays below exit_threshold for endConfirmFrames.
//
// Hysteresis on enter/exit thresholds plus minimum frame separation
// between events damp out numerical noise. Thresholds are normalised
// to the bone's own peak amplitude, so the same defaults work across
// rigs of any scale.
//
// Future detectors (footstep, etc.) plug in by implementing
// IMotionEventDetector and adding themselves to BuildRegistry() below.

#include "core/motion_events.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

// ---- RigidMechanismDetector -----------------------------------------------

class RigidMechanismDetector : public IMotionEventDetector {
 public:
  std::string Name() const override { return "rigid_mechanism"; }

  std::string Description() const override {
    return "Rigid mechanical motion (doors, levers, drawers, switches). "
           "Detects start / peak / zero-cross / end events from the bone's "
           "signed projection.";
  }

  std::vector<MotionEvent> Detect(
      const std::vector<float>& motion,
      double itemStartSec,
      double fps) const override {
    std::vector<MotionEvent> events;
    if (motion.size() < 3 || fps <= 0.0) return events;

    // Peak amplitude — basis for hysteresis thresholds. Entirely flat
    // signal => nothing to detect.
    float peak = 0.0f;
    for (float v : motion) {
      const float a = std::fabs(v);
      if (a > peak) peak = a;
    }
    if (peak < 1e-6f) return events;

    // Tunables. These ratios were validated on RiggedSimple-style hinge
    // motion at 24-60 fps; if they need to change for other rig classes,
    // expose as detector-construction params or as a separate detector.
    constexpr float kEnterRatio = 0.10f;  // 10% of peak to declare "active".
    constexpr float kExitRatio  = 0.05f;  //  5% of peak to leave "active".
    constexpr int   kMinSepFrames = 3;    // Min gap between events.
    constexpr int   kEndConfirmFrames = 3; // |v|<exit must persist this long.

    const float enterThreshold = kEnterRatio * peak;
    const float exitThreshold  = kExitRatio  * peak;

    // 3-frame moving average. Removes single-frame numerical spikes
    // without shifting extrema by more than ~1 frame, which is
    // imperceptible at 30+ fps.
    const std::size_t n = motion.size();
    std::vector<float> smoothed(n);
    for (std::size_t f = 0; f < n; ++f) {
      const std::size_t a = (f == 0) ? 0 : f - 1;
      const std::size_t b = (f + 1 < n) ? f + 1 : n - 1;
      smoothed[f] = (motion[a] + motion[f] + motion[b]) / 3.0f;
    }

    // Per-event-type debounce: track the last frame we emitted each
    // type, drop events that arrive too close together.
    int lastFrameForStart   = -kMinSepFrames * 2;
    int lastFrameForEnd     = -kMinSepFrames * 2;
    int lastFrameForExtremum = -kMinSepFrames * 2;
    int lastFrameForZero    = -kMinSepFrames * 2;

    auto emit = [&](std::size_t f, const std::string& type) {
      events.push_back({itemStartSec + static_cast<double>(f) / fps, type});
    };

    bool inActive = false;
    int belowExitCount = 0;

    for (std::size_t f = 0; f < n; ++f) {
      const float v = smoothed[f];
      const float a = std::fabs(v);

      // ---- Activity gate: start / end -----
      if (!inActive) {
        if (a > enterThreshold &&
            static_cast<int>(f) - lastFrameForStart >= kMinSepFrames) {
          inActive = true;
          belowExitCount = 0;
          emit(f, motion_event_types::kStart);
          lastFrameForStart = static_cast<int>(f);
        }
      } else {
        if (a < exitThreshold) {
          ++belowExitCount;
          if (belowExitCount >= kEndConfirmFrames) {
            // The actual end is the first frame of the under-threshold
            // run, not the confirmation frame; back the marker up so it
            // lands at the real motion offset.
            const std::size_t endFrame =
                f >= static_cast<std::size_t>(kEndConfirmFrames - 1)
                    ? f - (kEndConfirmFrames - 1)
                    : 0;
            if (static_cast<int>(endFrame) - lastFrameForEnd >= kMinSepFrames) {
              emit(endFrame, motion_event_types::kEnd);
              lastFrameForEnd = static_cast<int>(endFrame);
            }
            inActive = false;
            belowExitCount = 0;
          }
        } else {
          belowExitCount = 0;
        }
      }

      // ---- Zero crossing — fire even outside active phases so we catch
      // crossings at the very edge of motion. Uses raw sign change.
      if (f > 0) {
        const float prev = smoothed[f - 1];
        // Cross is the first frame on the new side; require strict sign
        // flip rather than including zero on both sides to avoid double
        // events at exactly v == 0.
        if ((prev < 0.0f && v >= 0.0f) || (prev > 0.0f && v <= 0.0f)) {
          if (static_cast<int>(f) - lastFrameForZero >= kMinSepFrames) {
            emit(f, motion_event_types::kZeroCross);
            lastFrameForZero = static_cast<int>(f);
          }
        }
      }

      // ---- Local extrema (peak_hi / peak_lo) — only inside active phase
      // (boundary frames produce noisy "extrema" that aren't meaningful).
      if (inActive && f > 0 && f + 1 < n) {
        const float prev = smoothed[f - 1];
        const float next = smoothed[f + 1];
        const bool isMax = v > prev && v > next;
        const bool isMin = v < prev && v < next;
        if ((isMax || isMin) &&
            static_cast<int>(f) - lastFrameForExtremum >= kMinSepFrames) {
          emit(f, isMax ? motion_event_types::kPeakHigh
                        : motion_event_types::kPeakLow);
          lastFrameForExtremum = static_cast<int>(f);
        }
      }
    }

    // Events were emitted in time-then-type order by construction (we
    // walk frames forward and emit at-most-once per frame across types),
    // but extrema and gates may interleave. Sort to guarantee
    // monotonically increasing timeSec for downstream consumers.
    std::sort(events.begin(), events.end(),
              [](const MotionEvent& a, const MotionEvent& b) {
                return a.timeSec < b.timeSec;
              });

    return events;
  }
};

// ---- Registry --------------------------------------------------------------

const std::vector<std::unique_ptr<IMotionEventDetector>>& Registry() {
  static const auto detectors = []() {
    std::vector<std::unique_ptr<IMotionEventDetector>> d;
    d.push_back(std::make_unique<RigidMechanismDetector>());
    // Future:
    //   d.push_back(std::make_unique<FootstepDetector>());
    //   d.push_back(std::make_unique<ImpactDetector>());
    return d;
  }();
  return detectors;
}

}  // namespace

const IMotionEventDetector* FindMotionEventDetector(const std::string& name) {
  for (const auto& d : Registry()) {
    if (d->Name() == name) return d.get();
  }
  return nullptr;
}

std::vector<std::string> AllMotionEventDetectorNames() {
  std::vector<std::string> names;
  names.reserve(Registry().size());
  for (const auto& d : Registry()) names.push_back(d->Name());
  return names;
}

const IMotionEventDetector* DefaultMotionEventDetector() {
  const auto& reg = Registry();
  return reg.empty() ? nullptr : reg.front().get();
}
