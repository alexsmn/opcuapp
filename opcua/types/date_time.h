#pragma once

// Standalone re-implementation of Chromium's base::Time (renamed
// opcua::DateTime here). API-compatible with the Chromium original so that
// ported code continues to compile unchanged.

#include <stdint.h>

#include <iosfwd>
#include <string>

#include "opcua/base/time_base.h"
#include "opcua/types/duration.h"

namespace opcua {
#ifdef _WIN32
// Forward-declare FILETIME to avoid including <windows.h>.
typedef struct _FILETIME FILETIME;
#endif

// Built-in OPC UA DateTime: a UTC instant, represented as a signed 64-bit
// count of 100-nanosecond intervals since 1601-01-01 00:00:00 UTC.
// OPC UA Part 6 §5.2.2.5:
// https://reference.opcfoundation.org/Core/Part6/v105/docs/5.2.2.5
class DateTime : public base::time_internal::TimeBase<DateTime, 10> {
 public:
  static constexpr int64_t kTicksPerMicrosecond = 10;
  static constexpr int64_t kTimeTToMicrosecondsOffset =
      INT64_C(11644473600000000);
  static constexpr int64_t kTimeTToDateTimeTicksOffset =
      kTimeTToMicrosecondsOffset * kTicksPerMicrosecond;

  struct Exploded {
    int year;
    int month;
    int day_of_week;
    int day_of_month;
    int hour;
    int minute;
    int second;
    int millisecond;

    bool HasValidValues() const;
  };

  constexpr DateTime() : TimeBase(0) {}

  static DateTime UnixEpoch();
  static DateTime Now();

  static DateTime FromDeltaSinceWindowsEpoch(Duration delta);
  Duration ToDeltaSinceWindowsEpoch() const;

  static DateTime FromDoubleT(double dt);
  double ToDoubleT() const;

  static bool FromUTCExploded(const Exploded& exploded, DateTime* time) {
    return FromExploded(false, exploded, time);
  }
  static bool FromLocalExploded(const Exploded& exploded, DateTime* time) {
    return FromExploded(true, exploded, time);
  }

  static bool FromString(const char* time_string, DateTime* parsed_time) {
    return FromStringInternal(time_string, true, parsed_time);
  }
  static bool FromUTCString(const char* time_string, DateTime* parsed_time) {
    return FromStringInternal(time_string, false, parsed_time);
  }

  void UTCExplode(Exploded* exploded) const { Explode(false, exploded); }
  void LocalExplode(Exploded* exploded) const { Explode(true, exploded); }

  DateTime LocalMidnight() const;

#ifdef _WIN32
  // FILETIME uses 100-nanosecond intervals since the same Windows epoch.
  static DateTime FromFileTime(FILETIME ft);
  FILETIME ToFileTime() const;
#endif

  // Constructs a DateTime from its OPC UA 100-nanosecond representation.
  static constexpr DateTime FromInternalValue(int64_t ticks) {
    return DateTime(ticks);
  }

 private:
  friend class base::time_internal::TimeBase<DateTime, kTicksPerMicrosecond>;

  constexpr explicit DateTime(int64_t ticks) : TimeBase(ticks) {}

  void Explode(bool is_local, Exploded* exploded) const;
  static bool FromExploded(bool is_local,
                           const Exploded& exploded,
                           DateTime* time);
  static bool FromStringInternal(const char* time_string,
                                 bool is_local,
                                 DateTime* parsed_time);
  static bool ExplodedMostlyEquals(const Exploded& lhs, const Exploded& rhs);
};

std::ostream& operator<<(std::ostream& os, DateTime time);

std::string ToString(DateTime time);
std::u16string ToString16(DateTime time);

}  // namespace opcua
