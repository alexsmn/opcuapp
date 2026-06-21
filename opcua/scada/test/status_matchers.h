#pragma once

#include "opcua/scada/status.h"
#include "opcua/scada/status_or.h"

#include <gmock/gmock.h>

#include <utility>

namespace opcua {
namespace test {

MATCHER_P(StatusIs, status_code, "") {
  if constexpr (requires { arg.status(); }) {
    return arg.status().code() == status_code;
  } else {
    return arg.code() == status_code;
  }
}

MATCHER(IsOk, "") {
  if constexpr (requires { arg.ok(); }) {
    if (!arg.ok()) {
      *result_listener << "which has status " << arg.status();
      return false;
    }
    return true;
  } else {
    if (!arg.good()) {
      *result_listener << "which is " << arg;
      return false;
    }
    return true;
  }
}

MATCHER_P(IsOkAndHolds, inner_matcher, "") {
  if (!arg.ok()) {
    *result_listener << "which has status " << arg.status();
    return false;
  }

  return ::testing::ExplainMatchResult(inner_matcher, *arg, result_listener);
}

}  // namespace opcua::test

#define EXPECT_OK(expr) EXPECT_THAT((expr), ::scada::test::IsOk())
#define ASSERT_OK(expr) ASSERT_THAT((expr), ::scada::test::IsOk())

#define SCADA_STATUS_MATCHERS_CONCAT_INNER(a, b) a##b
#define SCADA_STATUS_MATCHERS_CONCAT(a, b) \
  SCADA_STATUS_MATCHERS_CONCAT_INNER(a, b)

#define ASSERT_OK_AND_ASSIGN(lhs, expr)                                      \
  auto SCADA_STATUS_MATCHERS_CONCAT(status_or_value_, __LINE__) = (expr);   \
  ASSERT_OK(SCADA_STATUS_MATCHERS_CONCAT(status_or_value_, __LINE__));       \
  lhs = std::move(*SCADA_STATUS_MATCHERS_CONCAT(status_or_value_, __LINE__))
}  // namespace opcua (vendored)
