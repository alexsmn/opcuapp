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

// Built-in OPC UA DateTime: a UTC instant, carried as microseconds since the
// Windows epoch 1601-01-01 00:00:00 UTC. OPC UA Part 3 §8.11 DateTime
// (encoding: Part 6 §5.1.4),
// https://reference.opcfoundation.org/Core/Part3/v105/docs/8.11
class DateTime : public base::time_internal::TimeBase<DateTime> {
 public:
  static constexpr int64_t kTimeTToMicrosecondsOffset =
      INT64_C(11644473600000000);

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

  static constexpr DateTime FromInternalValue(int64_t us) {
    return DateTime(us);
  }

 private:
  friend class base::time_internal::TimeBase<DateTime>;

  constexpr explicit DateTime(int64_t us) : TimeBase(us) {}

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
