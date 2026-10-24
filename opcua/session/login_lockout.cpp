#include "opcua/session/login_lockout.h"

#include <algorithm>
#include <utility>

namespace opcua {

LoginLockout::LoginLockout(LoginLockoutPolicy policy,
                           std::function<DateTime()> now)
    : policy_{std::move(policy)}, now_{std::move(now)} {}

bool LoginLockout::IsLockedOut(std::span<const std::string> keys) const {
  if (policy_.max_failures == 0)
    return false;
  const DateTime now = now_();
  return std::ranges::any_of(keys, [&](const std::string& key) {
    auto it = keys_.find(key);
    return it != keys_.end() && IsLocked(it->second, now);
  });
}

bool LoginLockout::RecordFailure(std::span<const std::string> keys) {
  if (policy_.max_failures == 0)
    return false;
  const DateTime now = now_();
  bool locked = false;
  for (const auto& key : keys) {
    if (!keys_.contains(key))
      MakeRoom(now);
    auto& state = keys_[key];
    state.last_failure = now;
    if (IsLocked(state, now))
      continue;
    if (++state.failures >= policy_.max_failures) {
      state.locked_until = now + LockoutFor(state.lockouts);
      ++state.lockouts;
      state.failures = 0;
      locked = true;
    }
  }
  return locked;
}

void LoginLockout::RecordSuccess(std::span<const std::string> keys) {
  for (const auto& key : keys) {
    auto it = keys_.find(key);
    if (it != keys_.end() && !IsLocked(it->second, now_()))
      keys_.erase(it);
  }
}

bool LoginLockout::IsLocked(const KeyState& state, DateTime now) const {
  return !state.locked_until.is_null() && now < state.locked_until;
}

Duration LoginLockout::LockoutFor(std::size_t lockouts) const {
  Duration duration = policy_.base_lockout;
  for (std::size_t i = 0; i < lockouts && duration < policy_.max_lockout; ++i)
    duration = duration * 2;
  return std::min(duration, policy_.max_lockout);
}

void LoginLockout::MakeRoom(DateTime now) {
  if (keys_.size() < policy_.max_tracked_keys)
    return;
  // Forget keys that are not locked out, oldest failure first. A locked key is
  // kept, because dropping it is exactly how a lockout would be evaded.
  std::erase_if(keys_, [&](const auto& entry) {
    return !IsLocked(entry.second, now) &&
           now - entry.second.last_failure > policy_.base_lockout;
  });
  if (keys_.size() < policy_.max_tracked_keys)
    return;
  auto oldest = keys_.end();
  for (auto it = keys_.begin(); it != keys_.end(); ++it) {
    if (IsLocked(it->second, now))
      continue;
    if (oldest == keys_.end() ||
        it->second.last_failure < oldest->second.last_failure) {
      oldest = it;
    }
  }
  if (oldest != keys_.end())
    keys_.erase(oldest);
}

std::string PeerAddress(std::string_view peer) {
  if (peer.starts_with('[')) {
    const auto close = peer.find(']');
    return std::string{
        close == std::string_view::npos ? peer : peer.substr(1, close - 1)};
  }
  // An unbracketed address with more than one ':' is a bare IPv6 address with
  // no port; leave it whole.
  const auto colon = peer.rfind(':');
  if (colon == std::string_view::npos || peer.find(':') != colon)
    return std::string{peer};
  return std::string{peer.substr(0, colon)};
}

}  // namespace opcua
