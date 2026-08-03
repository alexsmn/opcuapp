#pragma once

#include "opcua/base/utf_convert.h"

#include <boost/log/common.hpp>
#include <boost/log/sources/logger.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/formatting_ostream.hpp>
#include <boost/log/utility/manipulators/add_value.hpp>
#include <optional>

// Allow streaming std::u16string to std::ostream.
inline std::ostream& operator<<(std::ostream& os, std::u16string_view sv) {
  os << opcua::UtfConvert<char>(sv);
  return os;
}
inline std::ostream& operator<<(std::ostream& os, const std::u16string& str) {
  os << opcua::UtfConvert<char>(std::u16string_view{str});
  return os;
}

// Overloads in Boost.Log namespace so ADL finds them for formatting_ostream.
namespace boost::log {
inline formatting_ostream& operator<<(formatting_ostream& os,
                                      std::u16string_view sv) {
  os << opcua::UtfConvert<char>(sv);
  return os;
}
inline formatting_ostream& operator<<(formatting_ostream& os,
                                      const std::u16string& str) {
  os << opcua::UtfConvert<char>(std::u16string_view{str});
  return os;
}
}  // namespace boost::log

using BoostLogSeverity = boost::log::trivial::severity_level;

using BoostLogger =
    boost::log::sources::severity_channel_logger_mt<BoostLogSeverity>;

namespace opcua {

// Log-record attribute carrying the current request's W3C traceparent
// (attach with `LOG_TAG(kTraceParentLogAttribute, <traceparent>)`). Contract
// with the embedding application's structured log sinks (the scada repo's
// metrics::kTraceParentLogAttribute): sinks parse this attribute into
// trace/span correlation fields instead of emitting it as a plain field, so
// the name must stay "TraceParent". Empty and non-traceparent values are
// harmless — structured sinks drop them, text sinks print non-empty values
// verbatim.
inline constexpr const char* kTraceParentLogAttribute = "TraceParent";

}  // namespace opcua

#define LOG_TRACE(logger) BOOST_LOG_SEV(logger, ::BoostLogSeverity::trace)
// Debug level is only logged in debug builds.
#define LOG_DEBUG(logger) BOOST_LOG_SEV(logger, ::BoostLogSeverity::debug)
#define LOG_INFO(logger) BOOST_LOG_SEV(logger, ::BoostLogSeverity::info)
#define LOG_WARNING(logger) BOOST_LOG_SEV(logger, ::BoostLogSeverity::warning)
#define LOG_ERROR(logger) BOOST_LOG_SEV(logger, ::BoostLogSeverity::error)
#define LOG_CRITICAL(logger) BOOST_LOG_SEV(logger, ::BoostLogSeverity::fatal)
#define LOG_SEV(logger, severity) BOOST_LOG_SEV(logger, severity)
#define LOG_STATUS(logger, status) \
  LOG_SEV(logger,                  \
          status ? ::BoostLogSeverity::info : ::BoostLogSeverity::warning)

#define LOG_TAG(name, value) boost::log::add_value(name, value)
#define LOG_SCOPED_TAG(logger, name, value) \
  BOOST_LOG_SCOPED_LOGGER_TAG(logger, name, value)
#define LOG_BIND_TAG(logger, name, value) \
  (logger).add_attribute((name), boost::log::attributes::make_constant(value))

#define LOG_NAME(name) boost::log::keywords::channel = (name)

std::optional<BoostLogSeverity> ParseLogSeverity(std::string_view str);
std::string_view ToStringView(BoostLogSeverity log_severity);
