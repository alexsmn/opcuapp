#pragma once

#include "opcua/types/date_time.h"

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace opcua {

// How LoginLockout reacts to repeated identity-token failures.
//
// OPC UA Part 4 §7.40.2.1 Overview: "Servers shall log details of any failure
// to validate a UserIdentityToken and shall lock out Client applications after
// five failures."
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.40.2.1
struct LoginLockoutPolicy {
  // Failures that lock a key out. Zero disables the lockout altogether.
  std::size_t max_failures = 5;
  // Length of the first lockout; each further lockout of the same key doubles
  // it, up to `max_lockout`.
  Duration base_lockout = Duration::FromMinutes(15);
  Duration max_lockout = Duration::FromHours(24);
  // Upper bound on tracked keys, so a stream of distinct peers cannot grow the
  // table without limit. When full, keys that are not locked out are dropped
  // first.
  std::size_t max_tracked_keys = 4096;
};

// Counts identity-token validation failures per key and locks a key out once
// it reaches the policy's limit.
//
// A caller presents every key that identifies the client, and a request is
// refused if ANY of them is locked out. Part 4 §5.7.3.1 ActivateSession names
// the keys: the client "is either detected by IP address for unsecured
// connections or by the ApplicationUri for secured connections"
// (https://reference.opcfoundation.org/Core/Part4/v105/docs/5.7.3.1). The
// session manager keys on the peer address always, and additionally on the
// client certificate on a secured channel: an ApplicationUri is text the client
// chooses, so keying on it alone would let an attacker reset its own counter by
// changing it, while the certificate is what the channel actually verified.
class LoginLockout {
 public:
  LoginLockout(LoginLockoutPolicy policy, std::function<DateTime()> now);

  // Whether any of `keys` is currently locked out.
  [[nodiscard]] bool IsLockedOut(std::span<const std::string> keys) const;

  // Records one failure against every key. Returns true when this failure
  // locked at least one of them out.
  bool RecordFailure(std::span<const std::string> keys);

  // A successful validation clears the failure count of every key. An active
  // lockout is not lifted early: a key that is locked out never reaches a
  // successful validation in the first place.
  void RecordSuccess(std::span<const std::string> keys);

  [[nodiscard]] std::size_t tracked_key_count() const { return keys_.size(); }

 private:
  struct KeyState {
    std::size_t failures = 0;
    // How many times this key has been locked out; doubles the next lockout.
    std::size_t lockouts = 0;
    DateTime locked_until;
    DateTime last_failure;
  };

  [[nodiscard]] bool IsLocked(const KeyState& state, DateTime now) const;
  [[nodiscard]] Duration LockoutFor(std::size_t lockouts) const;
  void MakeRoom(DateTime now);

  LoginLockoutPolicy policy_;
  std::function<DateTime()> now_;
  std::unordered_map<std::string, KeyState> keys_;
};

// The address half of a transport peer string ("address:port", IPv6 bracketed
// as "[::1]:4840"), which is what a lockout or a per-peer quota keys on: the
// port changes with every connection. Returns `peer` unchanged when it carries
// no port.
[[nodiscard]] std::string PeerAddress(std::string_view peer);

}  // namespace opcua
