#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/scada/authentication.h"

#include <memory>

namespace opcua {
namespace scada {

inline AsyncAuthenticator MakeAsyncAuthenticator(AnyExecutor executor,
                                                 Authenticator authenticator);

class FunctionCoroutineAuthenticator final : public CoroutineAuthenticator {
 public:
  explicit FunctionCoroutineAuthenticator(AsyncAuthenticator authenticator)
      : authenticator_{std::move(authenticator)} {}

  Awaitable<StatusOr<AuthenticationResult>> Authenticate(
      LocalizedText user_name,
      LocalizedText password) override {
    co_return co_await authenticator_(std::move(user_name),
                                      std::move(password));
  }

 private:
  AsyncAuthenticator authenticator_;
};

inline std::shared_ptr<CoroutineAuthenticator> MakeCoroutineAuthenticator(
    AsyncAuthenticator authenticator) {
  return std::make_shared<FunctionCoroutineAuthenticator>(
      std::move(authenticator));
}

inline std::shared_ptr<CoroutineAuthenticator> MakeCoroutineAuthenticator(
    AnyExecutor,
    Authenticator authenticator) {
  return MakeCoroutineAuthenticator(std::move(authenticator));
}

inline AsyncAuthenticator MakeAsyncAuthenticator(
    CoroutineAuthenticator& authenticator) {
  return [&authenticator](LocalizedText user_name,
                          LocalizedText password)
             -> Awaitable<StatusOr<AuthenticationResult>> {
    co_return co_await authenticator.Authenticate(std::move(user_name),
                                                  std::move(password));
  };
}

inline Authenticator MakeAuthenticator(AnyExecutor,
                                       AsyncAuthenticator async_authenticator) {
  return std::move(async_authenticator);
}

inline Authenticator MakeAuthenticator(AnyExecutor executor,
                                       CoroutineAuthenticator& authenticator) {
  return MakeAuthenticator(std::move(executor),
                           MakeAsyncAuthenticator(authenticator));
}

inline AsyncAuthenticator MakeAsyncAuthenticator(AnyExecutor,
                                                 Authenticator authenticator) {
  return std::move(authenticator);
}

}  // namespace scada
}  // namespace opcua (vendored)
