#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/base/struct_writer.h"
#include "opcua/types/localized_text.h"
#include "opcua/types/node_id.h"
#include "opcua/types/status.h"
#include "opcua/types/status_or.h"

#include <functional>

namespace opcua {

// opcuapp result of authenticating a user's UserIdentityToken: the resolved
// user NodeId, granted rights and whether concurrent sessions are allowed. OPC
// UA Part 4 §7.40 UserIdentityToken,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.40
struct AuthenticationResult {
  opcua::NodeId user_id;
  unsigned user_rights = 0;
  bool multi_sessions = false;
};

// opcuapp callback delivering an `AuthenticationResult`. OPC UA Part 4 §7.40
// UserIdentityToken,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.40
using AuthenticationCallback =
    std::function<void(const AuthenticationResult& result)>;

// opcuapp callable that verifies a UserName/password UserIdentityToken and
// resolves it to an `AuthenticationResult`. OPC UA Part 4 §7.40
// UserIdentityToken,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.40
// TODO: Merge into `SessionService`.
using Authenticator =
    std::function<Awaitable<opcua::StatusOr<AuthenticationResult>>(
        opcua::LocalizedText user_name,
        opcua::LocalizedText password)>;

// Alias of `Authenticator` retained for callers expecting the async name. OPC
// UA Part 4 §7.40 UserIdentityToken,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.40
using AsyncAuthenticator = Authenticator;

// opcuapp coroutine interface for verifying a UserName/password
// UserIdentityToken; the object-oriented form of `Authenticator`. OPC UA Part 4
// §7.40 UserIdentityToken,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.40
class CoroutineAuthenticator {
 public:
  virtual ~CoroutineAuthenticator() = default;

  virtual Awaitable<opcua::StatusOr<AuthenticationResult>> Authenticate(
      opcua::LocalizedText user_name,
      opcua::LocalizedText password) = 0;
};

inline std::ostream& operator<<(std::ostream& stream,
                                const AuthenticationResult& result) {
  StructWriter{stream}
      .AddField("user_id", result.user_id)
      .AddField("user_rights", result.user_rights)
      .AddField("multi_sessions", result.multi_sessions);
  return stream;
}

}  // namespace opcua
