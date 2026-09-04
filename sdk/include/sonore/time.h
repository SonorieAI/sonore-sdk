// SPDX-License-Identifier: Apache-2.0
//
// Dates, and lengths of time, as text.
//
// ── Why this exists now ─────────────────────────────────────────────────────
//
// files.h reports a modification time and nothing could show it. A preset
// browser with a Date column, a sampler saying a file is 3:24 long, a "last
// used" ordering -- all of them need the same two conversions and neither was
// here.
//
// ── Not std::chrono's formatting ────────────────────────────────────────────
//
// std::format and <chrono>'s calendar types are C++20, and this SDK is C++17 so
// a generated plugin builds with whatever toolchain its author already has.
// What is here is the handful of formats a plugin actually shows, done once.
//
// ── Thread safety, which localtime does not have ───────────────────────────
//
// std::localtime returns a pointer to a STATIC struct. Two threads formatting a
// date at the same time -- a background scan filling a browser while the UI
// draws it -- get each other's answers, and the bug looks like a date that is
// occasionally somebody else's. localtime_s and localtime_r are the reentrant
// forms and this uses whichever the platform has.
#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace sonore {

/** A moment, split up. Fields are what a human would say: month 1..12, not
 *  0..11, because every off-by-one month bug starts with tm_mon. */
struct DateTime {
  int year = 1970;
  int month = 1;   // 1..12
  int day = 1;     // 1..31
  int hour = 0;    // 0..23
  int minute = 0;
  int second = 0;
  /** 0 = Sunday. */
  int weekday = 0;
};

/** Seconds since the Unix epoch, now. */
inline int64_t currentUnixTime() { return (int64_t) std::time(nullptr); }

namespace timedetail {

/** The reentrant localtime. See the header for why the ordinary one is a bug
 *  waiting for a second thread. */
inline bool breakDown(int64_t unixSeconds, bool utc, std::tm* out) {
  if (!out) return false;
  const std::time_t t = (std::time_t) unixSeconds;
#if defined(_WIN32)
  return (utc ? gmtime_s(out, &t) : localtime_s(out, &t)) == 0;
#else
  return (utc ? gmtime_r(&t, out) : localtime_r(&t, out)) != nullptr;
#endif
}

} // namespace timedetail

inline DateTime dateTimeFromUnix(int64_t unixSeconds, bool utc = false) {
  DateTime out;
  std::tm parts{};
  if (!timedetail::breakDown(unixSeconds, utc, &parts)) return out;
  out.year = parts.tm_year + 1900;
  out.month = parts.tm_mon + 1;
  out.day = parts.tm_mday;
  out.hour = parts.tm_hour;
  out.minute = parts.tm_min;
  out.second = parts.tm_sec;
  out.weekday = parts.tm_wday;
  return out;
}

/**
 * "2026-08-23", which sorts as text in the same order it does as a date.
 *
 * ISO rather than a local convention, and that is the point: a browser sorting
 * a Date column alphabetically gets the right answer, where "23/08/2026" sorts
 * by day and "08/23/2026" by month. A plugin that wants the user's own format
 * has DateTime and can build it.
 */
inline std::string formatDate(const DateTime& when) {
  // 32, not 16. These are ints, and "%04d" of a full int is eleven characters
  // -- so a DateTime somebody built by hand rather than from the clock could
  // silently produce a truncated date. snprintf never overruns, which is why
  // this was only ever a wrong STRING and not a crash, and why it sat behind a
  // -Wformat-truncation warning in every build instead of being noticed. A
  // warning that is always there is a warning nobody reads.
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", when.year, when.month, when.day);
  return buffer;
}

/** "14:07". Seconds left off: a preset's minute is as much as anybody reads. */
inline std::string formatClockTime(const DateTime& when, bool withSeconds = false) {
  char buffer[32]; // see formatDate: three ints, not three two-digit numbers

  if (withSeconds)
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", when.hour, when.minute, when.second);
  else
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", when.hour, when.minute);
  return buffer;
}

/** "2026-08-23 14:07". */
inline std::string formatDateTime(const DateTime& when, bool withSeconds = false) {
  return formatDate(when) + " " + formatClockTime(when, withSeconds);
}

/**
 * A LENGTH, as a musician reads it: "3:24", or "1:02:03" past an hour.
 *
 * Not a date -- this is how long a sample is, and it is the format every
 * player in the world uses. Rounded to the nearest second rather than
 * truncated: a 3.7-second sample reading "0:03" and then playing for four is
 * the kind of small wrongness that makes people distrust a readout.
 */
inline std::string formatDuration(double seconds) {
  const bool negative = seconds < 0.0;
  if (negative) seconds = -seconds;
  const int64_t whole = (int64_t) (seconds + 0.5);
  const int64_t hours = whole / 3600;
  const int64_t minutes = (whole / 60) % 60;
  const int64_t remainder = whole % 60;
  char buffer[32];
  if (hours > 0)
    std::snprintf(buffer, sizeof(buffer), "%s%lld:%02lld:%02lld", negative ? "-" : "",
                  (long long) hours, (long long) minutes, (long long) remainder);
  else
    std::snprintf(buffer, sizeof(buffer), "%s%lld:%02lld", negative ? "-" : "",
                  (long long) minutes, (long long) remainder);
  return buffer;
}

/**
 * A short length, for anything under a minute: "0.35 s", "12 ms".
 *
 * Which is what a latency readout, a decay time or an attack wants -- 0:00 is
 * true of every one of them and useful for none.
 */
inline std::string formatShortDuration(double seconds) {
  char buffer[32];
  const double magnitude = seconds < 0.0 ? -seconds : seconds;
  if (magnitude < 0.001) std::snprintf(buffer, sizeof(buffer), "%.0f µs", seconds * 1e6);
  else if (magnitude < 1.0) std::snprintf(buffer, sizeof(buffer), "%.1f ms", seconds * 1e3);
  else std::snprintf(buffer, sizeof(buffer), "%.2f s", seconds);
  return buffer;
}

/**
 * How long ago, roughly: "just now", "12 minutes ago", "3 days ago".
 *
 * Deliberately coarse. A browser column reading "2 hours ago" is what somebody
 * scanning for the preset they made this morning is actually looking for, and
 * a timestamp to the second is a number they have to subtract in their head.
 */
inline std::string describeAge(int64_t then, int64_t now) {
  int64_t delta = now - then;
  if (delta < 0) return "in the future"; // a file from a machine with a wrong clock
  if (delta < 45) return "just now";

  char buffer[48];
  if (delta < 3600) {
    const int64_t minutes = (delta + 30) / 60;
    std::snprintf(buffer, sizeof(buffer), "%lld minute%s ago", (long long) minutes,
                  minutes == 1 ? "" : "s");
    return buffer;
  }
  if (delta < 86400) {
    const int64_t hours = delta / 3600;
    std::snprintf(buffer, sizeof(buffer), "%lld hour%s ago", (long long) hours,
                  hours == 1 ? "" : "s");
    return buffer;
  }
  if (delta < 86400 * 30) {
    const int64_t days = delta / 86400;
    std::snprintf(buffer, sizeof(buffer), "%lld day%s ago", (long long) days,
                  days == 1 ? "" : "s");
    return buffer;
  }
  if (delta < 86400 * 365) {
    const int64_t months = delta / (86400 * 30);
    std::snprintf(buffer, sizeof(buffer), "%lld month%s ago", (long long) months,
                  months == 1 ? "" : "s");
    return buffer;
  }
  const int64_t years = delta / (86400 * 365);
  std::snprintf(buffer, sizeof(buffer), "%lld year%s ago", (long long) years,
                years == 1 ? "" : "s");
  return buffer;
}

} // namespace sonore
