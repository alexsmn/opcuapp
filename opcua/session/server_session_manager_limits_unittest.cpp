#include "opcua/session/server_session_manager.h"

#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"
#include "opcua/session/authentication_adapters.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

// Session-layer protections for a listener exposed to untrusted peers: the
// identity-token lockout (OPC UA Part 4 §7.40.2.1,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.40.2.1), the
// anonymous-only mode, the session caps (§5.7.2.3) and the activation window.

namespace opcua {
namespace {

class ServerSessionManagerLimitsTest : public testing::Test {
 protected:
  ServerSessionManagerContext MakeContext() {
    return {
        .authenticator = MakeCoroutineAuthenticator(
            [this](LocalizedText user_name, LocalizedText password)
                -> Awaitable<StatusOr<AuthenticationResult>> {
              ++authenticate_calls_;
              if (password != LocalizedText{u"secret"})
                co_return StatusOr<AuthenticationResult>{
                    Status{StatusCode::Bad_IdentityTokenRejected}};
              co_return AuthenticationResult{.user_id = NodeId{42, 4},
                                             .multi_sessions = true};
            }),
        .on_audit_event =
            [this](const SessionAuditEvent& event) {
              audit_messages_.push_back(event.message);
            },
        .now = [this] { return now_; },
        .min_timeout = Duration::FromSeconds(10),
    };
  }

  CreateSessionResponse Create(ServerSessionManager& manager,
                               std::string peer = "203.0.113.7:50000") {
    return WaitAwaitable(executor_,
                         manager.CreateSession({.peer = std::move(peer)}));
  }

  ActivateSessionResponse Login(ServerSessionManager& manager,
                                std::u16string password,
                                std::string peer = "203.0.113.7:50000") {
    const auto created = Create(manager, peer);
    EXPECT_TRUE(created.status.good());
    return WaitAwaitable(
        executor_, manager.ActivateSession(
                       {.authentication_token = created.authentication_token,
                        .user_name = LocalizedText{u"operator"},
                        .password = LocalizedText{std::move(password)},
                        .peer = std::move(peer)}));
  }

  DateTime now_ = DateTime::Now();
  TestExecutor executor_;
  int authenticate_calls_ = 0;
  std::vector<std::string> audit_messages_;
};

TEST_F(ServerSessionManagerLimitsTest, FiveBadPasswordsLockThePeerOut) {
  ServerSessionManager manager{MakeContext()};
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(Login(manager, u"wrong").status.code(),
              StatusCode::Bad_IdentityTokenRejected);
  }
  // The right password no longer helps, and the authenticator is not asked.
  const int calls_before = authenticate_calls_;
  EXPECT_EQ(Login(manager, u"secret").status.code(),
            StatusCode::Bad_UserAccessDenied);
  EXPECT_EQ(authenticate_calls_, calls_before);
  EXPECT_NE(std::ranges::find(audit_messages_,
                              "client locked out after repeated failures"),
            audit_messages_.end());

  // Another address is unaffected.
  EXPECT_TRUE(Login(manager, u"secret", "198.51.100.1:1234").status.good());
}

TEST_F(ServerSessionManagerLimitsTest, LockoutEndsAfterItsDuration) {
  ServerSessionManager manager{MakeContext()};
  for (int i = 0; i < 5; ++i)
    Login(manager, u"wrong");
  now_ = now_ + Duration::FromMinutes(16);
  // Sessions created before the jump have expired; a fresh login succeeds.
  EXPECT_TRUE(Login(manager, u"secret").status.good());
}

TEST_F(ServerSessionManagerLimitsTest, SuccessResetsTheFailureCount) {
  ServerSessionManager manager{MakeContext()};
  for (int i = 0; i < 4; ++i)
    Login(manager, u"wrong");
  ASSERT_TRUE(Login(manager, u"secret").status.good());
  for (int i = 0; i < 4; ++i)
    Login(manager, u"wrong");
  EXPECT_TRUE(Login(manager, u"secret").status.good());
}

TEST_F(ServerSessionManagerLimitsTest, AnonymousOnlyRefusesUserTokens) {
  auto context = MakeContext();
  context.anonymous_only = true;
  ServerSessionManager manager{std::move(context)};

  EXPECT_EQ(Login(manager, u"secret").status.code(),
            StatusCode::Bad_IdentityTokenRejected);
  EXPECT_EQ(authenticate_calls_, 0);

  const auto created = Create(manager);
  const auto anonymous = WaitAwaitable(
      executor_, manager.ActivateSession(
                     {.authentication_token = created.authentication_token,
                      .allow_anonymous = true,
                      .peer = "203.0.113.7:50000"}));
  EXPECT_TRUE(anonymous.status.good());
  EXPECT_FALSE(anonymous.authentication_result.has_value());
}

TEST_F(ServerSessionManagerLimitsTest, SessionCapRefusesTheNextSession) {
  auto context = MakeContext();
  context.max_sessions = 2;
  ServerSessionManager manager{std::move(context)};
  EXPECT_TRUE(Create(manager, "10.0.0.1:1").status.good());
  EXPECT_TRUE(Create(manager, "10.0.0.2:1").status.good());
  EXPECT_EQ(Create(manager, "10.0.0.3:1").status.code(),
            StatusCode::Bad_TooManySessions);
}

TEST_F(ServerSessionManagerLimitsTest, PerPeerCapCountsTheAddressNotThePort) {
  auto context = MakeContext();
  context.max_sessions_per_peer = 2;
  ServerSessionManager manager{std::move(context)};
  EXPECT_TRUE(Create(manager, "10.0.0.1:1000").status.good());
  EXPECT_TRUE(Create(manager, "10.0.0.1:1001").status.good());
  EXPECT_EQ(Create(manager, "10.0.0.1:1002").status.code(),
            StatusCode::Bad_TooManySessions);
  EXPECT_TRUE(Create(manager, "10.0.0.2:1000").status.good());
}

TEST_F(ServerSessionManagerLimitsTest,
       UnactivatedSessionExpiresAfterTheWindow) {
  auto context = MakeContext();
  context.activation_timeout = Duration::FromSeconds(10);
  context.max_sessions = 1;
  ServerSessionManager manager{std::move(context)};

  const auto idle = Create(manager);
  ASSERT_TRUE(idle.status.good());
  now_ = now_ + Duration::FromSeconds(11);
  // The idle session no longer holds the only slot.
  const auto next = Create(manager);
  EXPECT_TRUE(next.status.good());
  EXPECT_FALSE(manager.FindSession(idle.authentication_token).has_value());
}

TEST_F(ServerSessionManagerLimitsTest, ActivatedSessionKeepsItsFullTimeout) {
  auto context = MakeContext();
  context.activation_timeout = Duration::FromSeconds(10);
  ServerSessionManager manager{std::move(context)};

  const auto created = Create(manager);
  ASSERT_TRUE(WaitAwaitable(executor_, manager.ActivateSession(
                                           {.authentication_token =
                                                created.authentication_token,
                                            .allow_anonymous = true}))
                  .status.good());
  now_ = now_ + Duration::FromSeconds(60);
  manager.PruneExpiredSessions();
  EXPECT_TRUE(manager.FindSession(created.authentication_token).has_value());
}

TEST_F(ServerSessionManagerLimitsTest, FailureWaitsOutTheConfiguredDelay) {
  std::vector<Duration> slept;
  auto context = MakeContext();
  context.failure_delay = Duration::FromSeconds(1);
  context.sleep = [&slept](Duration delay) -> Awaitable<void> {
    slept.push_back(delay);
    co_return;
  };
  ServerSessionManager manager{std::move(context)};

  EXPECT_TRUE(Login(manager, u"secret").status.good());
  EXPECT_TRUE(slept.empty());
  EXPECT_FALSE(Login(manager, u"wrong").status.good());
  EXPECT_EQ(slept, std::vector<Duration>{Duration::FromSeconds(1)});
}

}  // namespace
}  // namespace opcua
