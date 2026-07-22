#pragma once

#include <cstdint>
#include <ostream>
#include <string>

namespace opcua {

// Severity field of a StatusCode (Good, Uncertain, Bad, or Reserved), held in
// the two most significant bits of the code. OPC UA Part 4 §7.38 StatusCode,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.38
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

// Built-in OPC UA StatusCode: a numeric code describing the result of an
// operation. The constants below mix standard codes with
// opcuapp/vendor-specific ones. OPC UA Part 4 §7.38 StatusCode,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.38
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
  // Bad codes with a standard OPC UA equivalent carry the standard name and
  // SubCode (OPC UA Part 6 Annex A / Opc.Ua.StatusCodes.csv,
  // https://files.opcfoundation.org/schemas/UA/1.04/Opc.Ua.StatusCodes.csv);
  // `Status` shifts the enumerator left by 16, so `Bad_Foo` goes on the wire
  // as exactly the standard `BadFoo` code. Codes with no standard equivalent
  // keep their vendor names and live in the vendor-extension SubCode range
  // 0x2000+, which the standard does not define — spec-conforming clients
  // fall back to the severity bits instead of misreading them as unrelated
  // standard codes.
  Bad_IdentityTokenRejected = Bad | 0x21,
  Bad_UserIsAlreadyLoggedOn = Bad | 0x2005,
  Bad_ProtocolVersionUnsupported = Bad | 0xBE,
  // Another command is already executing on the object.
  Bad_ResourceUnavailable = Bad | 0x04,
  Bad_NodeIdUnknown = Bad | 0x34,
  Bad_WrongDeviceId = Bad | 0x2006,
  // Trying to perform command on disconnected object.
  Bad_NoCommunication = Bad | 0x31,
  // The session was terminated because the same user reconnected.
  Bad_SessionClosed = Bad | 0x26,
  Bad_Timeout = Bad | 0x0A,
  Bad_CantDeleteDependentNode = Bad | 0x2001,
  Bad_Shutdown = Bad | 0x0C,
  Bad_MethodInvalid = Bad | 0x75,
  Bad_CantDeleteOwnUser = Bad | 0x2007,
  Bad_NodeIdExists = Bad | 0x5E,
  Bad_UnsupportedFileVersion = Bad | 0x2002,
  Bad_TypeDefinitionInvalid = Bad | 0x63,
  Bad_ParentNodeIdInvalid = Bad | 0x5B,
  Bad_SessionIdInvalid = Bad | 0x25,
  Bad_SubscriptionIdInvalid = Bad | 0x28,
  Bad_ContinuationPointInvalid = Bad | 0x4A,
  Bad_Iec60870UnknownType = Bad | 0x2010,
  Bad_Iec60870UnknownCot = Bad | 0x2011,
  Bad_Iec60870UnknownDevice = Bad | 0x2012,
  Bad_Iec60870UnknownAddress = Bad | 0x2013,
  Bad_Iec60870UnknownError = Bad | 0x2014,
  Bad_InvalidArgument = Bad | 0xAB,
  // A string value could not be parsed into the attribute type.
  Bad_TypeMismatch = Bad | 0x74,
  // A supplied string exceeds the maximum accepted length.
  Bad_OutOfRange = Bad | 0x3C,
  Bad_WrongPropertyId = Bad | 0x2008,
  Bad_ReferenceTypeIdInvalid = Bad | 0x4C,
  Bad_NodeClassInvalid = Bad | 0x5F,
  Bad_AttributeIdInvalid = Bad | 0x35,
  Bad_Iec61850Error = Bad | 0x2015,
  Bad_NothingToDo = Bad | 0x0F,
  Bad_BrowseNameInvalid = Bad | 0x60,
  Bad_TargetNodeIdInvalid = Bad | 0x65,
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
  // The HistoryRead details parameter is not valid (e.g. a raw read with no
  // time range and no continuation point) — OPC UA Part 11 §6.4 HistoryRead,
  // https://reference.opcfoundation.org/Core/Part11/v105/docs/6.4
  Bad_HistoryOperationInvalid = Bad | 0x71,
  // There is no subscription available for this session — OPC UA Part 4 §5.13.5
  // Publish, https://reference.opcfoundation.org/Core/Part4/v105/docs/5.13.5
  Bad_NoSubscription = Bad | 0x79,
  // The server does not support the requested service — OPC UA Part 4 §7.34
  // ServiceFault, https://reference.opcfoundation.org/Core/Part4/v105/docs/7.34
  Bad_ServiceUnsupported = Bad | 0x0B,
  // The user identity was authenticated but is not authorized for the
  // requested operation (BadUserAccessDenied) — OPC UA Part 4 §5.7.3,
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/5.7.3. Distinct
  // from Bad_IdentityTokenRejected (BadIdentityTokenRejected), which means
  // the authentication itself failed.
  Bad_UserAccessDenied = Bad | 0x1F,
  // The requested operation is not supported by this implementation
  // (BadNotSupported) — OPC UA Part 4 §7.39 Common StatusCodes,
  // https://reference.opcfoundation.org/Core/Part4/v105/docs/7.39
  Bad_NotSupported = Bad | 0x3D,
};

// Limit bits of a StatusCode, indicating whether the value is at a low/high
// limit or constant. OPC UA Part 4 §7.38 StatusCode,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.38
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

// Wrapper around a full 32-bit OPC UA StatusCode, exposing its severity, limit
// bits, and the base StatusCode value. OPC UA Part 4 §7.38 StatusCode,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.38
class Status {
 public:
  // Default-constructs to Good, matching the spec's default for a StatusCode
  // field. Needed so that Status can be an element of a decoded array.
  constexpr Status() noexcept : full_code_(0) {}

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

const char* ToCString(opcua::StatusCode status_code);

std::string ToString(opcua::StatusCode status_code);
std::u16string ToString16(opcua::StatusCode status_code);

std::string ToString(const opcua::Status& status);
std::u16string ToString16(const opcua::Status& status);

inline std::ostream& operator<<(std::ostream& stream, StatusCode status_code) {
  return stream << ToString(status_code);
}

inline std::ostream& operator<<(std::ostream& stream, const Status& status) {
  return stream << ToString(status);
}

}  // namespace opcua
