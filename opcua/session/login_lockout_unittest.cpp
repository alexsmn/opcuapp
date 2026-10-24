#include "opcua/session/login_lockout.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace opcua {
namespace {

class LoginLockoutTest : public ::testing::Test {
 protected:
  LoginLockout MakeLockout(LoginLockoutPolicy policy = {}) {
    return LoginLockout{policy, [this] { return now_; }};
  }

  DateTime now_ = DateTime::UnixEpoch() + Duration::FromDays(20000);
  const std::vector<std::string> ip_ = {"ip:203.0.113.7"};
};

TEST_F(LoginLockoutTest, FiveFailuresLockTheKeyOut) {
  auto lockout = MakeLockout();
  for (int i = 0; i < 4; ++i) {
    EXPECT_FALSE(lockout.RecordFailure(ip_));
    EXPECT_FALSE(lockout.IsLockedOut(ip_));
  }
  EXPECT_TRUE(lockout.RecordFailure(ip_));
  EXPECT_TRUE(lockout.IsLockedOut(ip_));
}

TEST_F(LoginLockoutTest, LockoutExpiresAfterTheBaseDuration) {
  auto lockout = MakeLockout();
  for (int i = 0; i < 5; ++i)
    lockout.RecordFailure(ip_);
  now_ = now_ + Duration::FromMinutes(14);
  EXPECT_TRUE(lockout.IsLockedOut(ip_));
  now_ = now_ + Duration::FromMinutes(1);
  EXPECT_FALSE(lockout.IsLockedOut(ip_));
}

TEST_F(LoginLockoutTest, RepeatedLockoutsDoubleUpToTheMaximum) {
  auto lockout = MakeLockout({.base_lockout = Duration::FromHours(10),
                              .max_lockout = Duration::FromHours(24)});
  for (int i = 0; i < 5; ++i)
    lockout.RecordFailure(ip_);
  now_ = now_ + Duration::FromHours(10);
  ASSERT_FALSE(lockout.IsLockedOut(ip_));

  // Second lockout: 20 h.
  for (int i = 0; i < 5; ++i)
    lockout.RecordFailure(ip_);
  now_ = now_ + Duration::FromHours(19);
  EXPECT_TRUE(lockout.IsLockedOut(ip_));
  now_ = now_ + Duration::FromHours(1);
  ASSERT_FALSE(lockout.IsLockedOut(ip_));

  // Third lockout would be 40 h; capped at 24 h.
  for (int i = 0; i < 5; ++i)
    lockout.RecordFailure(ip_);
  now_ = now_ + Duration::FromHours(23);
  EXPECT_TRUE(lockout.IsLockedOut(ip_));
  now_ = now_ + Duration::FromHours(1);
  EXPECT_FALSE(lockout.IsLockedOut(ip_));
}

TEST_F(LoginLockoutTest, SuccessResetsTheCount) {
  auto lockout = MakeLockout();
  for (int i = 0; i < 4; ++i)
    lockout.RecordFailure(ip_);
  lockout.RecordSuccess(ip_);
  for (int i = 0; i < 4; ++i)
    lockout.RecordFailure(ip_);
  EXPECT_FALSE(lockout.IsLockedOut(ip_));
}

TEST_F(LoginLockoutTest, AnyLockedKeyRefusesTheRequest) {
  auto lockout = MakeLockout();
  const std::vector<std::string> cert = {"cert:abcd"};
  for (int i = 0; i < 5; ++i)
    lockout.RecordFailure(cert);
  // Same certificate from a fresh address: still locked out, because one of
  // the presented keys is.
  const std::vector<std::string> both = {"ip:198.51.100.1", "cert:abcd"};
  EXPECT_TRUE(lockout.IsLockedOut(both));
  EXPECT_FALSE(
      lockout.IsLockedOut(std::vector<std::string>{"ip:198.51.100.1"}));
}

TEST_F(LoginLockoutTest, ZeroMaxFailuresDisablesTheLockout) {
  auto lockout = MakeLockout({.max_failures = 0});
  for (int i = 0; i < 50; ++i)
    EXPECT_FALSE(lockout.RecordFailure(ip_));
  EXPECT_FALSE(lockout.IsLockedOut(ip_));
}

TEST_F(LoginLockoutTest, TableStaysBoundedAndKeepsLockedKeys) {
  auto lockout = MakeLockout({.max_tracked_keys = 8});
  for (int i = 0; i < 5; ++i)
    lockout.RecordFailure(ip_);
  for (int i = 0; i < 100; ++i) {
    lockout.RecordFailure(
        std::vector<std::string>{"ip:10.0.0." + std::to_string(i)});
  }
  EXPECT_LE(lockout.tracked_key_count(), 8u);
  EXPECT_TRUE(lockout.IsLockedOut(ip_));
}

TEST(PeerAddressTest, StripsThePort) {
  EXPECT_EQ(PeerAddress("203.0.113.7:51234"), "203.0.113.7");
  EXPECT_EQ(PeerAddress("[2001:db8::1]:4850"), "2001:db8::1");
  EXPECT_EQ(PeerAddress("2001:db8::1"), "2001:db8::1");
  EXPECT_EQ(PeerAddress("host"), "host");
  EXPECT_EQ(PeerAddress(""), "");
}

}  // namespace
}  // namespace opcua
