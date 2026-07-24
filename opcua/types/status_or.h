#pragma once

#include "opcua/base/panic.h"
#include "opcua/types/status.h"

#include <concepts>
#include <expected>
#include <ostream>
#include <source_location>
#include <type_traits>
#include <utility>

namespace opcua {

// Value-or-status result type built on std::expected<T, Status>. Unlike raw
// std::expected it keeps the project error-handling contract:
//  - implicit construction from Status/StatusCode, so error propagation
//    (`return status;`, `co_return StatusCode::Bad_X;`) needs no
//    std::unexpected wrapping at call sites;
//  - fail-stop accessors: value()/operator*/operator->/error() panic via
//    opcua::base::Panic in every build type instead of throwing
//    (std::expected::value) or being undefined (std::expected::operator*);
//  - the invariant that a value-less StatusOr always carries a non-good
//    status, enforced at construction.
// The std::expected base additionally provides has_value(), value_or() and the
// monadic operations (and_then/transform/or_else); their std::expected results
// convert back to StatusOr implicitly. Mirrors scada::StatusOr in
// core/scada/status_or.h.
template <class T>
class [[nodiscard]] StatusOr : public std::expected<T, Status> {
  using Base = std::expected<T, Status>;

 public:
  StatusOr(Status status) : Base{std::unexpect, std::move(status)} {
    CheckErrorStatusIsBad();
  }

  StatusOr(StatusCode status_code) : StatusOr{Status(status_code)} {}

  StatusOr(T value) : Base{std::in_place, std::move(value)} {}

  // Implicit conversion from the base type, so std::expected results of the
  // monadic operations flow back into StatusOr-returning code. Constrained to
  // exactly std::expected<T, Status> so it never competes with the value and
  // status constructors during overload resolution (e.g. constructing
  // StatusOr<std::unique_ptr<B>> from std::unique_ptr<D>).
  template <class U>
    requires std::same_as<std::remove_cvref_t<U>, std::expected<T, Status>>
  StatusOr(U&& base) : Base{std::forward<U>(base)} {
    if (!this->has_value()) {
      CheckErrorStatusIsBad();
    }
  }

  bool ok() const { return this->has_value(); }

  Status status() const {
    return this->has_value() ? Status{StatusCode::Good} : this->Base::error();
  }

  T& value(
      const std::source_location& location = std::source_location::current()) {
    CheckValuePresent(location);
    return this->Base::operator*();
  }

  const T& value(const std::source_location& location =
                     std::source_location::current()) const {
    CheckValuePresent(location);
    return this->Base::operator*();
  }

  T& operator*() {
    CheckValuePresent(std::source_location::current());
    return this->Base::operator*();
  }

  const T& operator*() const {
    CheckValuePresent(std::source_location::current());
    return this->Base::operator*();
  }

  T* operator->() {
    CheckValuePresent(std::source_location::current());
    return this->Base::operator->();
  }

  const T* operator->() const {
    CheckValuePresent(std::source_location::current());
    return this->Base::operator->();
  }

  // Shadows std::expected's templated value_or with a concrete-typed parameter
  // so callers can pass a braced default: `value_or({})`.
  T value_or(T fallback) && {
    return this->has_value() ? std::move(this->Base::operator*())
                             : std::move(fallback);
  }

  // Equality is defined directly on StatusOr instead of relying on the
  // std::expected hidden friends: without these exact-match overloads,
  // overload resolution picks expected's expected-vs-value operator== with the
  // value type deduced as StatusOr itself, whose body then converts the
  // contained value back into an expected and recurses infinitely at runtime.
  friend bool operator==(const StatusOr& lhs, const StatusOr& rhs) {
    if (lhs.has_value() != rhs.has_value()) {
      return false;
    }
    return lhs.has_value() ? lhs.Base::operator*() == rhs.Base::operator*()
                           : lhs.Base::error() == rhs.Base::error();
  }

  friend bool operator==(const StatusOr& lhs, const T& value) {
    return lhs.has_value() && lhs.Base::operator*() == value;
  }

  // Checked counterpart of std::expected::error(), which is undefined when a
  // value is present.
  const Status& error(const std::source_location& location =
                          std::source_location::current()) const {
    if (this->has_value()) {
      opcua::base::Panic("StatusOr error access with a value present",
                         location);
    }
    return this->Base::error();
  }

 private:
  void CheckErrorStatusIsBad() const {
    if (this->Base::error()) {
      opcua::base::Panic(
          "StatusOr constructed without a value from an ok status");
    }
  }

  void CheckValuePresent(const std::source_location& location) const {
    if (!this->has_value()) {
      opcua::base::Panic("StatusOr value access without a value", location);
    }
  }
};

template <class T>
inline std::ostream& operator<<(std::ostream& os,
                                const opcua::StatusOr<T>& st) {
  return st.ok() ? (os << *st) : (os << st.status());
}
}  // namespace opcua
