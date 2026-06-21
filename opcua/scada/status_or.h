#pragma once

#include "opcua/base/panic.h"
#include "opcua/scada/status.h"

#include <ostream>
#include <source_location>
#include <utility>
#include <variant>

namespace opcua {

template <class T>
class [[nodiscard]] StatusOr {
 public:
  StatusOr(Status status) : value_{std::move(status)} {
    if (this->status()) {
      opcua::base::Panic("StatusOr constructed without a value from an ok status");
    }
  }

  StatusOr(StatusCode status_code) : StatusOr{Status(status_code)} {}

  StatusOr(T value) : value_{std::move(value)} {}

  bool ok() const { return std::holds_alternative<T>(value_); }

  Status status() const {
    const auto* status = std::get_if<Status>(&value_);
    return status ? *status : Status{StatusCode::Good};
  }

  T& value(const std::source_location& location =
               std::source_location::current()) {
    CheckValuePresent(location);
    return std::get<T>(value_);
  }

  const T& value(const std::source_location& location =
                     std::source_location::current()) const {
    CheckValuePresent(location);
    return std::get<T>(value_);
  }

  T& operator*() {
    CheckValuePresent(std::source_location::current());
    return std::get<T>(value_);
  }

  const T& operator*() const {
    CheckValuePresent(std::source_location::current());
    return std::get<T>(value_);
  }

  T value_or(T fallback) && {
    return ok() ? std::move(value()) : std::move(fallback);
  }

  T* operator->() {
    CheckValuePresent(std::source_location::current());
    return &std::get<T>(value_);
  }

  const T* operator->() const {
    CheckValuePresent(std::source_location::current());
    return &std::get<T>(value_);
  }

 private:
  using StatusVariant = std::variant<Status, T>;

  void CheckValuePresent(const std::source_location& location) const {
    if (!ok()) {
      opcua::base::Panic("StatusOr value access without a value", location);
    }
  }

  StatusVariant value_;
};


template <class T>
inline std::ostream& operator<<(std::ostream& os,
                                const opcua::StatusOr<T>& st) {
  return st.ok() ? (os << *st) : (os << st.status());
}
}  // namespace opcua (vendored)
