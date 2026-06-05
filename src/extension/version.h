// v0.13.0-alpha.1: single source of truth for the currently-built
// HoloRoll version. Used by the in-app auto-updater to decide
// whether the latest GitHub release is newer than what's installed.
//
// **Bump this string every time you tag a new release.** The
// alpha-suffix is significant (alpha.10 != alpha.1, and pre-release
// versions sort below the corresponding clean release per semver).
//
// Format: MAJOR.MINOR.PATCH[-pre.N]
//   "0.13.0-alpha.1"   — pre-release alpha 1 of 0.13.0
//   "0.13.0-beta.3"    — pre-release beta 3 of 0.13.0
//   "0.13.0"           — clean release (highest sort for that x.y.z)
//
// CompareVersions() in updater.cpp handles the ordering.

#pragma once

#define HOLOROLL_VERSION_STRING "0.16.0-alpha.4"
