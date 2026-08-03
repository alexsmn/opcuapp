#include "opcua/types/status_or.h"

#include <expected>
#include <memory>
#include <utility>

#include <gtest/gtest.h>

namespace opcua {
namespace {

// Construction and the ok()/status() contract.

TEST(StatusOrTest, HoldsAValue) {
  const StatusOr<int> result{42};
  EXPECT_TRUE(result.ok());
  EXPECT_TRUE(result.has_value());
  EXPECT_TRUE(result.status().good());
  EXPECT_EQ(*result, 42);
  EXPECT_EQ(result.value(), 42);
}

TEST(StatusOrTest, CarriesABadStatus) {
  const StatusOr<int> result{StatusCode::Bad_Timeout};
  EXPECT_FALSE(result.ok());
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.status().code(), StatusCode::Bad_Timeout);
  EXPECT_EQ(result.error().code(), StatusCode::Bad_Timeout);
}

// The std::expected surface the rebase brings in.

TEST(StatusOrTest, ValueOrTakesABracedDefault) {
  EXPECT_EQ(StatusOr<int>{7}.value_or(5), 7);
  EXPECT_EQ(StatusOr<int>{StatusCode::Bad}.value_or(5), 5);
}

TEST(StatusOrTest, Equality) {
  EXPECT_EQ(StatusOr<int>{7}, StatusOr<int>{7});
  EXPECT_NE(StatusOr<int>{7}, StatusOr<int>{8});
  EXPECT_NE(StatusOr<int>{7}, StatusOr<int>{StatusCode::Bad});
}

TEST(StatusOrTest, MonadicTransform) {
  const StatusOr<int> doubled =
      StatusOr<int>{21}.transform([](int value) { return value * 2; });
  ASSERT_TRUE(doubled.ok());
  EXPECT_EQ(*doubled, 42);
}

TEST(StatusOrTest, MonadicAndThenPropagatesError) {
  const StatusOr<int> result = StatusOr<int>{StatusCode::Bad_Timeout}.and_then(
      [](int value) { return std::expected<int, Status>{value}; });
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::Bad_Timeout);
}

// std::expected results of the monadic operations flow back implicitly.

TEST(StatusOrTest, ConstructsFromExpectedValue) {
  const std::expected<int, Status> expected{42};
  const StatusOr<int> result{expected};
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(*result, 42);
}

TEST(StatusOrTest, ConstructsFromExpectedBadStatus) {
  const std::expected<int, Status> expected{std::unexpect,
                                            Status{StatusCode::Bad_Timeout}};
  const StatusOr<int> result{expected};
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::Bad_Timeout);
}

// A move-only payload survives construction and unwrapping (the arrow operator
// forwards through the std::expected base).
TEST(StatusOrTest, HoldsAMoveOnlyPayload) {
  StatusOr<std::unique_ptr<int>> result{std::make_unique<int>(9)};
  ASSERT_TRUE(result.ok());
  ASSERT_NE(result->get(), nullptr);
  EXPECT_EQ(**result, 9);
}

#if GTEST_HAS_DEATH_TEST
// The fail-stop contract: accessors panic rather than throw or be undefined.

TEST(StatusOrTest, ValueAccessWithoutValuePanics) {
  const StatusOr<int> result{StatusCode::Bad};
  EXPECT_DEATH(
      { (void)result.value(); },
      "Panic: StatusOr value access without a value\r?\n.*status_or_unittest");
}

TEST(StatusOrTest, ConstructingBadFromOkStatusPanics) {
  EXPECT_DEATH(
      {
        const StatusOr<int> result{StatusCode::Good};
        (void)result;
      },
      "Panic: StatusOr constructed without a value from an ok status.*"
      "\r?\n.*status_or.h");
}

TEST(StatusOrTest, ErrorAccessWithValuePanics) {
  const StatusOr<int> result{1};
  EXPECT_DEATH(
      { (void)result.error(); },
      "Panic: StatusOr error access with a value present\r?\n.*"
      "status_or_unittest");
}
#endif  // GTEST_HAS_DEATH_TEST

}  // namespace
}  // namespace opcua
