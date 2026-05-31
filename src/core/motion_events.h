// v0.12.0-alpha.9: Motion event detection — pluggable algorithm interface.
//
// Algorithms turn a per-frame motion curve (alpha.8 signed projection along
// the joint's principal axis) into a sequence of timestamped EVENTS:
// motion-start, peak, zero-crossing, motion-end, etc. The events are then
// realised as REAPER project markers by the placement code in entry.cpp.
//
// First concrete algorithm: RigidMechanismDetector — tuned for doors,
// levers, drawers, and other "purposeful" mechanical motion. Future
// algorithms (footstep detection for biped locomotion, peak-density
// detection for impact-heavy clips) will plug in by implementing
// IMotionEventDetector and registering with the same registry.
//
// The detection layer is pure / engine-agnostic: takes float arrays,
// returns event timestamps. No REAPER, no UI. That makes it trivially
// testable and lets us swap algorithms later without touching the
// REAPER-marker writing code.
#pragma once

#include <string>
#include <vector>

// Canonical event-type identifiers. String constants instead of enum so
// future detectors can introduce new types without recompiling everyone.
namespace motion_event_types {
constexpr char kStart[]     = "start";       // Motion onset (rest -> moving).
constexpr char kEnd[]       = "end";         // Motion offset (moving -> rest).
constexpr char kPeakHigh[]  = "peak_hi";     // Local maximum of signed motion.
constexpr char kPeakLow[]   = "peak_lo";     // Local minimum of signed motion.
constexpr char kZeroCross[] = "zero_cross";  // Signed motion crosses 0.
}  // namespace motion_event_types

// One detected event in absolute project-time space.
struct MotionEvent {
  double timeSec;        // Absolute project time (= itemStartSec + frame/fps).
  std::string eventType; // One of motion_event_types::kXxx.
};

// Detector contract. Implementations are STATELESS and PURE — same input
// always produces the same output, no side effects.
class IMotionEventDetector {
 public:
  virtual ~IMotionEventDetector() = default;

  // Stable identifier (lowercase ASCII, snake_case). Used in config keys
  // and UI labels: "rigid_mechanism", "footstep", etc.
  virtual std::string Name() const = 0;

  // Short human-readable description for tooltips / dropdown captions.
  virtual std::string Description() const = 0;

  // Run detection on a single bone's per-frame signed-motion curve.
  //
  // Parameters:
  //   motion        — per-frame values (alpha.8 signed-projection metric).
  //                   May be empty / static; detector returns no events.
  //   itemStartSec  — project time at which motion[0] sits.
  //   fps           — frames-per-second the motion was baked at.
  //
  // Returned events are sorted by timeSec ascending.
  virtual std::vector<MotionEvent> Detect(
      const std::vector<float>& motion,
      double itemStartSec,
      double fps) const = 0;
};

// Look up a detector by Name(). Returns nullptr if not registered.
const IMotionEventDetector* FindMotionEventDetector(const std::string& name);

// All registered detector names, in registration order. The first entry
// is the project-wide default.
std::vector<std::string> AllMotionEventDetectorNames();

// Convenience: the default detector (first registered). Never returns
// nullptr in a valid build — at least RigidMechanismDetector is always
// registered.
const IMotionEventDetector* DefaultMotionEventDetector();
