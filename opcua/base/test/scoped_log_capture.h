#pragma once

// Captures everything written to the global Boost.Log core for the object's
// lifetime, so a test can assert on the exact text a subsystem emits.
//
// `LOG_TAG` values are Boost.Log *attributes*, not part of the message, so the
// default text-ostream formatter drops them silently -- a capture would show
// `OPC UA response read failed` and no `Endpoint`, and a test asserting on a
// tag would fail while production logged it correctly. This installs a
// formatter that appends `| Name = Value` for each one, which is the shape the
// embedding application's console and file sinks write.
//
// This is a copy of `scada-server-framework/base/test/scoped_log_capture.h`,
// kept in step by hand -- change one, change both. It cannot be shared: the
// framework consumes opcuapp, so opcuapp reaching into the framework for a test
// helper would be a cycle. That is the same reason `opcua/base/test/
// scoped_temp_dir.h` is a copy rather than `common`'s (see the root CLAUDE.md,
// Unit Test Guidance). Nothing in the tree generalises the two; whoever edits
// the formatter below edits the framework's too.

#include "opcua/base/boost_log.h"

#include <boost/log/attributes/attribute_name.hpp>
#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/make_shared.hpp>
#include <boost/shared_ptr.hpp>

#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>

namespace opcua {

class ScopedLogCapture {
 public:
  ScopedLogCapture() {
    auto backend =
        boost::make_shared<boost::log::sinks::text_ostream_backend>();
    backend->add_stream(
        boost::shared_ptr<std::ostream>{stream_, stream_.get()});
    backend->auto_flush(true);
    sink_ = boost::make_shared<Sink>(backend);
    sink_->set_formatter(&FormatRecord);
    boost::log::core::get()->add_sink(sink_);
  }

  ~ScopedLogCapture() { boost::log::core::get()->remove_sink(sink_); }

  ScopedLogCapture(const ScopedLogCapture&) = delete;
  ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;

  // Everything logged since construction. Safe to call while the sink is still
  // installed; `auto_flush` above is what makes that true.
  std::string str() const { return stream_->str(); }

 private:
  using Sink = boost::log::sinks::synchronous_sink<
      boost::log::sinks::text_ostream_backend>;

  // The message, then every attribute that is not part of the record's own
  // plumbing. Severity, channel, line id and timestamp are deliberately left
  // out: they are noise for assertions, and none of them is what a caller of
  // this class is trying to observe.
  static void FormatRecord(const boost::log::record_view& record,
                           boost::log::formatting_ostream& stream) {
    // Narrow types only. The wide overloads would need `base/utf_convert.h`,
    // and no tag a test asserts on is a wide string -- `LOG_TAG` values in
    // this tree are endpoints, status names and counts.
    using Types =
        boost::mpl::vector<bool, std::int16_t, std::uint16_t, std::int32_t,
                           std::uint32_t, std::int64_t, std::uint64_t, long,
                           float, double, std::string>;
    namespace names = boost::log::aux::default_attribute_names;
    std::string tags;
    for (const auto& attr : record.attribute_values()) {
      std::string value;
      boost::log::visit<Types>(attr.second, boost::log::save_result(
                                                [](const auto& v) {
                                                  std::ostringstream out;
                                                  out << v;
                                                  return out.str();
                                                },
                                                value));
      if (value.empty()) {
        continue;
      }
      if (attr.first == names::message()) {
        stream << value;
      } else if (attr.first != names::severity() &&
                 attr.first != names::channel() &&
                 attr.first != names::line_id() &&
                 attr.first != names::timestamp()) {
        tags += " | " + attr.first.string() + " = " + value;
      }
    }
    stream << tags;
  }

  boost::shared_ptr<std::ostringstream> stream_ =
      boost::make_shared<std::ostringstream>();
  boost::shared_ptr<Sink> sink_;
};

}  // namespace opcua
