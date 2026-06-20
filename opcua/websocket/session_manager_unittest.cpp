#include "opcua/server_session_manager.h"

#include "opcua/scada/authentication_adapters.h"
#include "opcua/base/test/awaitable_test.h"
#include "opcua/base/test/test_executor.h"

#include <gtest/gtest.h>

using namespace testing;

namespace opcua::ws {
namespace {

class SessionManagerTest : public Test {
 protected:
  static opcua::scada::NodeId NumericNode(opcua::scada::NumericId id,
                                   opcua::scada::NamespaceIndex ns = 2) {
    return {id, ns};
  }

  opcua::base::Time now_ = opcua::base::Time::Now();
  opcua::TestExecutor executor_;

  auto MakeManager(std::shared_ptr<opcua::scada::CoroutineAuthenticator> authenticator,
                   opcua::base::TimeDelta default_timeout =
                       opcua::base::TimeDelta::FromMinutes(10)) {
    return ServerSessionManager{{
        .authenticator = std::move(authenticator),
        .now = [this] { return now_; },
        .default_timeout = default_timeout,
        .min_timeout = opcua::base::TimeDelta::FromSeconds(10),
        .max_timeout = opcua::base::TimeDelta::FromHours(1),
    }};
  }
};

TEST_F(SessionManagerTest, CreateActivateDetachResumeAndClose) {
  const auto expected_user_id = opcua::scada::NodeId{42, 4};
  auto manager = MakeManager(opcua::scada::MakeCoroutineAuthenticator(
      [expected_user_id](opcua::scada::LocalizedText user_name,
                         opcua::scada::LocalizedText password)
          -> opcua::Awaitable<opcua::scada::StatusOr<opcua::scada::AuthenticationResult>> {
        EXPECT_EQ(user_name, opcua::scada::LocalizedText{u"operator"});
        EXPECT_EQ(password, opcua::scada::LocalizedText{u"secret"});
        co_return opcua::scada::AuthenticationResult{.user_id = expected_user_id,
                                              .user_rights = 7,
                                              .multi_sessions = false};
      }));

  const auto created = opcua::WaitAwaitable(executor_, manager.CreateSession({}));
  EXPECT_EQ(created.status.code(), opcua::scada::StatusCode::Good);
  EXPECT_FALSE(created.session_id.is_null());
  EXPECT_FALSE(created.authentication_token.is_null());
  EXPECT_FALSE(created.server_nonce.empty());

  auto activated = opcua::WaitAwaitable(
      executor_,
      manager.ActivateSession({
          .session_id = created.session_id,
          .authentication_token = created.authentication_token,
          .user_name = opcua::scada::LocalizedText{u"operator"},
          .password = opcua::scada::LocalizedText{u"secret"},
      }));
  EXPECT_EQ(activated.status.code(), opcua::scada::StatusCode::Good);
  EXPECT_FALSE(activated.resumed);
  ASSERT_TRUE(activated.authentication_result.has_value());
  EXPECT_EQ(activated.authentication_result->user_id, expected_user_id);
  EXPECT_EQ(activated.service_context.user_id(), expected_user_id);

  auto session = manager.FindSession(created.authentication_token);
  ASSERT_TRUE(session.has_value());
  EXPECT_TRUE(session->activated);
  EXPECT_TRUE(session->attached);

  manager.DetachSession(created.authentication_token);
  session = manager.FindSession(created.authentication_token);
  ASSERT_TRUE(session.has_value());
  EXPECT_FALSE(session->attached);

  activated = opcua::WaitAwaitable(executor_,
                            manager.ActivateSession({
                                .session_id = created.session_id,
                                .authentication_token =
                                    created.authentication_token,
                            }));
  EXPECT_EQ(activated.status.code(), opcua::scada::StatusCode::Good);
  EXPECT_TRUE(activated.resumed);
  EXPECT_EQ(activated.service_context.user_id(), expected_user_id);

  const auto closed = manager.CloseSession(
      {.session_id = created.session_id,
       .authentication_token = created.authentication_token});
  EXPECT_EQ(closed.status.code(), opcua::scada::StatusCode::Good);
  EXPECT_FALSE(manager.FindSession(created.authentication_token).has_value());
}

TEST_F(SessionManagerTest, ActivateMissingSessionRejected) {
  auto manager = MakeManager(opcua::scada::MakeCoroutineAuthenticator(
      [](opcua::scada::LocalizedText, opcua::scada::LocalizedText)
          -> opcua::Awaitable<opcua::scada::StatusOr<opcua::scada::AuthenticationResult>> {
        co_return opcua::scada::AuthenticationResult{};
      }));

  const auto response = opcua::WaitAwaitable(
      executor_,
      manager.ActivateSession({
          .session_id = NumericNode(1),
          .authentication_token = NumericNode(2, 3),
      }));
  EXPECT_EQ(response.status.code(), opcua::scada::StatusCode::Bad_SessionIsLoggedOff);
}

TEST_F(SessionManagerTest, PendingSessionTimeoutIsPruned) {
  auto manager = MakeManager(opcua::scada::MakeCoroutineAuthenticator(
                                 [](opcua::scada::LocalizedText, opcua::scada::LocalizedText)
                                     -> opcua::Awaitable<opcua::scada::StatusOr<
                                         opcua::scada::AuthenticationResult>> {
                                   co_return opcua::scada::AuthenticationResult{};
                                 }),
                             opcua::base::TimeDelta::FromSeconds(30));

  const auto created = opcua::WaitAwaitable(
      executor_,
      manager.CreateSession(
          {.requested_timeout = opcua::base::TimeDelta::FromSeconds(15)}));
  EXPECT_TRUE(manager.FindSession(created.authentication_token).has_value());

  now_ = now_ + opcua::base::TimeDelta::FromSeconds(16);
  manager.PruneExpiredSessions();
  EXPECT_FALSE(manager.FindSession(created.authentication_token).has_value());
}

TEST_F(SessionManagerTest, AnonymousActivationUsesRevisedTimeout) {
  const auto null_user_id = opcua::scada::NodeId{};
  auto manager = MakeManager(opcua::scada::MakeCoroutineAuthenticator(
      [](opcua::scada::LocalizedText, opcua::scada::LocalizedText)
          -> opcua::Awaitable<opcua::scada::StatusOr<opcua::scada::AuthenticationResult>> {
        ADD_FAILURE() << "Authenticator should not run for anonymous activation";
        co_return opcua::scada::AuthenticationResult{};
      }));

  const auto created =
      opcua::WaitAwaitable(executor_,
                    manager.CreateSession(
                        {.requested_timeout = opcua::base::TimeDelta::FromSeconds(1)}));
  EXPECT_EQ(created.revised_timeout, opcua::base::TimeDelta::FromSeconds(10));

  const auto activated = opcua::WaitAwaitable(
      executor_,
      manager.ActivateSession({
          .session_id = created.session_id,
          .authentication_token = created.authentication_token,
          .allow_anonymous = true,
  }));
  EXPECT_EQ(activated.status.code(), opcua::scada::StatusCode::Good);
  EXPECT_FALSE(activated.authentication_result.has_value());
  EXPECT_EQ(activated.service_context.user_id(), null_user_id);

  now_ = now_ + opcua::base::TimeDelta::FromSeconds(9);
  manager.PruneExpiredSessions();
  EXPECT_TRUE(manager.FindSession(created.authentication_token).has_value());

  now_ = now_ + opcua::base::TimeDelta::FromSeconds(2);
  manager.PruneExpiredSessions();
  EXPECT_FALSE(manager.FindSession(created.authentication_token).has_value());
}

TEST_F(SessionManagerTest, ExpiredActivatedSessionCannotResume) {
  auto manager = MakeManager(opcua::scada::MakeCoroutineAuthenticator(
                                 [](opcua::scada::LocalizedText,
                                    opcua::scada::LocalizedText)
                                     -> opcua::Awaitable<opcua::scada::StatusOr<
                                         opcua::scada::AuthenticationResult>> {
                                   co_return opcua::scada::AuthenticationResult{
                                       .user_id = opcua::scada::NodeId{55, 6},
                                       .multi_sessions = true};
                                 }),
                             opcua::base::TimeDelta::FromSeconds(30));

  const auto created = opcua::WaitAwaitable(
      executor_,
      manager.CreateSession(
          {.requested_timeout = opcua::base::TimeDelta::FromSeconds(12)}));
  const auto activated = opcua::WaitAwaitable(
      executor_,
      manager.ActivateSession({
          .session_id = created.session_id,
          .authentication_token = created.authentication_token,
          .user_name = opcua::scada::LocalizedText{u"user"},
          .password = opcua::scada::LocalizedText{u"pass"},
      }));
  EXPECT_EQ(activated.status.code(), opcua::scada::StatusCode::Good);

  manager.DetachSession(created.authentication_token);
  now_ = now_ + opcua::base::TimeDelta::FromSeconds(13);
  manager.PruneExpiredSessions();

  const auto resumed = opcua::WaitAwaitable(
      executor_,
      manager.ActivateSession({
          .session_id = created.session_id,
          .authentication_token = created.authentication_token,
      }));
  EXPECT_EQ(resumed.status.code(), opcua::scada::StatusCode::Bad_SessionIsLoggedOff);
}

TEST_F(SessionManagerTest, SingleSessionUsersRequireDeleteExisting) {
  auto manager = MakeManager(opcua::scada::MakeCoroutineAuthenticator(
      [](opcua::scada::LocalizedText, opcua::scada::LocalizedText)
          -> opcua::Awaitable<opcua::scada::StatusOr<opcua::scada::AuthenticationResult>> {
        co_return opcua::scada::AuthenticationResult{
            .user_id = opcua::scada::NodeId{77, 8}, .multi_sessions = false};
      }));

  const auto first = opcua::WaitAwaitable(executor_, manager.CreateSession({}));
  const auto first_activated = opcua::WaitAwaitable(
      executor_,
      manager.ActivateSession({
          .session_id = first.session_id,
          .authentication_token = first.authentication_token,
          .user_name = opcua::scada::LocalizedText{u"user"},
          .password = opcua::scada::LocalizedText{u"pass"},
      }));
  EXPECT_EQ(first_activated.status.code(), opcua::scada::StatusCode::Good);

  const auto second = opcua::WaitAwaitable(executor_, manager.CreateSession({}));
  const auto rejected = opcua::WaitAwaitable(
      executor_,
      manager.ActivateSession({
          .session_id = second.session_id,
          .authentication_token = second.authentication_token,
          .user_name = opcua::scada::LocalizedText{u"user"},
          .password = opcua::scada::LocalizedText{u"pass"},
      }));
  EXPECT_EQ(rejected.status.code(), opcua::scada::StatusCode::Bad_UserIsAlreadyLoggedOn);

  const auto replaced = opcua::WaitAwaitable(
      executor_,
      manager.ActivateSession({
          .session_id = second.session_id,
          .authentication_token = second.authentication_token,
          .user_name = opcua::scada::LocalizedText{u"user"},
          .password = opcua::scada::LocalizedText{u"pass"},
          .delete_existing = true,
      }));
  EXPECT_EQ(replaced.status.code(), opcua::scada::StatusCode::Good);
  EXPECT_FALSE(manager.FindSession(first.authentication_token).has_value());
  EXPECT_TRUE(manager.FindSession(second.authentication_token).has_value());
}

}  // namespace
}  // namespace opcua::ws
