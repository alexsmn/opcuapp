#pragma once

#include <cstdint>
#include <ostream>
#include <string>

namespace opcua {
namespace scada {

enum class StatusSeverity {
  // Indicates that the operation was successful and the associated results may
  // be used.
  Good = 0,

  // Indicates that the operation was partially successful and that associated
  // results might not be suitable for some purposes.
  Uncertain = 1,

  // Indicates that the operation failed and any associated results cannot be
  // used.
  Bad = 2,

  // Reserved for future use. All Clients should treat a StatusCode with this
  // severity as Bad.
  Reserved = 3
};

enum class StatusCode : unsigned {
  Good = static_cast<unsigned>(StatusSeverity::Good),
  Good_Pending = Good | 1,  // Async operation started
  Good_Sporadic = Good | 2,
  Good_Backup = Good | 3,     // Uncertain_SubNormal
  Good_Manual = Good | 4,     // Good_LocalOverride
  Good_Simulated = Good | 5,  // Good_LocalOverride
  Uncertain = static_cast<unsigned>(StatusSeverity::Uncertain) << 14,
  Uncertain_DeviceFlag = Uncertain | 1,
  Uncertain_Misconfigured = Uncertain | 2,
  Uncertain_Disconnected = Uncertain | 3,
  Uncertain_NotUpdated = Uncertain | 4,
  // Lock command was not changed state, because object is already
  // locked/unlocked.
  Uncertain_StateWasNotChanged = Uncertain | 5,
  Bad = static_cast<unsigned>(StatusSeverity::Bad) << 14,
  Bad_WrongLoginCredentials = Bad | 0x1F,
  Bad_UserIsAlreadyLoggedOn = Bad | 2,
  Bad_UnsupportedProtocolVersion = Bad | 3,
  Bad_ObjectIsBusy = Bad | 4,
  Bad_WrongNodeId = Bad | 0x34,
  Bad_WrongDeviceId = Bad | 6,
  // Trying to perform command on disconnected object.
  Bad_Disconnected = Bad | 7,
  Bad_SessionForcedLogoff = Bad | 8,
  Bad_Timeout = Bad | 0x0A,
  Bad_CantDeleteDependentNode = Bad | 0x2001,
  Bad_ServerWasShutDown = Bad | 0x0C,  // Bad_Shutdown
  Bad_WrongMethodId = Bad | 0x75,
  Bad_CantDeleteOwnUser = Bad | 13,
  Bad_DuplicateNodeId = Bad | 14,
  Bad_UnsupportedFileVersion = Bad | 0x2002,
  Bad_WrongTypeId = Bad | 0x2003,
  Bad_WrongParentId = Bad | 17,
  Bad_SessionIsLoggedOff = Bad | 0x25,
  Bad_WrongSubscriptionId = Bad | 0x28,
  Bad_WrongIndex = Bad | 0x4A,
  Bad_Iec60870UnknownType = Bad | 21,
  Bad_Iec60870UnknownCot = Bad | 22,
  Bad_Iec60870UnknownDevice = Bad | 23,
  Bad_Iec60870UnknownAddress = Bad | 24,
  Bad_Iec60870UnknownError = Bad | 25,
  Bad_WrongCallArguments = Bad | 0xAB,
  Bad_CantParseString = Bad | 27,
  Bad_TooLongString = Bad | 28,
  Bad_WrongPropertyId = Bad | 29,
  Bad_WrongReferenceId = Bad | 30,
  Bad_WrongNodeClass = Bad | 0x2004,
  Bad_WrongAttributeId = Bad | 0x35,
  Bad_Iec61850Error = Bad | 33,
  Bad_NothingToDo = Bad | 0x0F,
  Bad_BrowseNameInvalid = Bad | 0x60,
  Bad_WrongTargetId = Bad | 36,
  Bad_MonitoredItemIdInvalid = Bad | 0x42,
  Bad_MessageNotAvailable = Bad | 0x7B,
  // The ActivateSession clientSignature did not verify against the client
  // application instance certificate (OPC UA Part 4 §5.6.3).
  Bad_ApplicationSignatureInvalid = Bad | 0x58,
  // The request contained more operations than the server permits (the
  // OperationLimits exposed in the address space, OPC UA Part 4 §5.10).
  Bad_TooManyOperations = Bad | 0x10,
  // CreateMonitoredItems requested more items than MaxMonitoredItemsPerCall.
  Bad_TooManyMonitoredItems = Bad | 0xDB,
  // A Publish acknowledgement referenced a sequence number the server does not
  // hold (unknown or already acknowledged) — OPC UA Part 4 §5.13.5 Publish,
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.5
  Bad_SequenceNumberUnknown = Bad | 0x7A,
  // The server has reached its maximum number of Browse continuation points and
  // cannot allocate another — OPC UA Part 4 §5.8.2 Browse,
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/5.8.2
  Bad_NoContinuationPoints = Bad | 0x4B,
  // The TimestampsToReturn enumeration of a Read/HistoryRead is out of range —
  // OPC UA Part 4 §7.40 TimestampsToReturn,
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/7.40
  Bad_TimestampsToReturnInvalid = Bad | 0x2B,
  // The Browse view (ViewDescription.viewId) is not known to the server —
  // OPC UA Part 4 §5.8.2 Browse,
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/5.8.2
  Bad_ViewIdUnknown = Bad | 0x6B,
  // The HistoryRead details parameter is not valid (e.g. a raw read with no time
  // range and no continuation point) — OPC UA Part 11 §6.4 HistoryRead,
  // https://reference.opcfoundation.org/Core/Part11/v105/docs/6.4
  Bad_HistoryOperationInvalid = Bad | 0x71,
  // There is no subscription available for this session — OPC UA Part 4 §5.13.5
  // Publish, https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.5
  Bad_NoSubscription = Bad | 0x79,
  // The server does not support the requested service — OPC UA Part 4 §7.34
  // ServiceFault, https://reference.opcfoundation.org/Core/Part4/v105/docs/7.34
  Bad_ServiceUnsupported = Bad | 0x0B,
};

enum class StatusLimit {
  // The value is free to change.
  None = 0,

  // The value is at the lower limit for the data source.
  Low = 1,

  // The value is at the higher limit for the data source.
  High = 2,

  // The value is constant and cannot change.
  Constant,
};

inline constexpr StatusSeverity GetSeverity(StatusCode code) noexcept {
  return static_cast<StatusSeverity>(static_cast<unsigned>(code) >> 14);
}

inline constexpr bool IsGood(StatusCode code) noexcept {
  return GetSeverity(code) == StatusSeverity::Good;
}

inline constexpr bool IsBad(StatusCode code) noexcept {
  return GetSeverity(code) == StatusSeverity::Bad;
}

class Status {
 public:
  constexpr Status(StatusCode code) noexcept
      : full_code_(static_cast<unsigned>(code) << 16) {}

  static Status FromFullCode(unsigned full_code);

  explicit constexpr operator bool() const noexcept { return !bad(); }
  constexpr bool operator!() const noexcept { return bad(); }

  constexpr StatusSeverity severity() const noexcept {
    return static_cast<StatusSeverity>(full_code_ >> 30);
  }

  constexpr bool good() const noexcept {
    return severity() == StatusSeverity::Good;
  }
  constexpr bool uncertain() const noexcept {
    return severity() == StatusSeverity::Uncertain;
  }
  constexpr bool bad() const noexcept {
    return severity() == StatusSeverity::Bad;
  }

  StatusLimit limit() const noexcept {
    return static_cast<StatusLimit>(full_code_ & 3);
  }

  void set_limit(StatusLimit limit) noexcept {
    full_code_ &= ~3U;
    full_code_ |= static_cast<unsigned>(limit);
  }

  constexpr StatusCode code() const noexcept {
    return static_cast<StatusCode>(full_code_ >> 16);
  }

  constexpr unsigned full_code() const noexcept { return full_code_; }

  constexpr bool operator==(const Status& other) const noexcept {
    return full_code_ == other.full_code_;
  }

  constexpr bool operator!=(const Status& other) const noexcept {
    return !operator==(other);
  }

 private:
  unsigned full_code_;
};

}  // namespace scada

const char* ToCString(opcua::scada::StatusCode status_code);

std::string ToString(opcua::scada::StatusCode status_code);
std::u16string ToString16(opcua::scada::StatusCode status_code);

std::string ToString(const opcua::scada::Status& status);
std::u16string ToString16(const opcua::scada::Status& status);

namespace scada {

inline std::ostream& operator<<(std::ostream& stream, StatusCode status_code) {
  return stream << ToString(status_code);
}

inline std::ostream& operator<<(std::ostream& stream, const Status& status) {
  return stream << ToString(status);
}

}  // namespace scada
}  // namespace opcua (vendored)
