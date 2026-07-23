#pragma once

#include "opcua/message.h"
#include "opcua/ua/ua_types.h"

// Converts between the generated, spec-conformant OPC UA subscription /
// monitored-item / publish wire types (`ua::CreateSubscriptionRequest` & co.)
// and the hand-written vocabulary in opcua/message.h.
//
// These hand-written types are the subscription engine's and the client publish
// pump's vocabulary: server_subscription builds `NotificationMessage` /
// `NotificationData` / `MonitoringParameters`, the client pump walks them, and
// the runtime dispatch traffics in the Request/Response variant. So — like the
// session and discovery services — this is a codec-internal cutover: the wire
// encoding is derived from the schema while the engine, pump, and runtime are
// untouched, and these helpers are the seam both codecs share.
//
// Only the message *body* fields are mapped; the request/response header is
// reconciled separately by the codec's header seam.
namespace opcua::subscription_conversion {

// --- Server side: decode wire request (ua) into the managed request. ---

CreateSubscriptionRequest ToManaged(const ua::CreateSubscriptionRequest& wire);
ModifySubscriptionRequest ToManaged(const ua::ModifySubscriptionRequest& wire);

// --- Server side: encode the managed response into the wire response (ua). ---

ua::CreateSubscriptionResponse ToWire(
    const CreateSubscriptionResponse& managed);
ua::ModifySubscriptionResponse ToWire(
    const ModifySubscriptionResponse& managed);

// --- Client side: encode the managed request into the wire request (ua). ---

ua::CreateSubscriptionRequest ToWire(const CreateSubscriptionRequest& managed);
ua::ModifySubscriptionRequest ToWire(const ModifySubscriptionRequest& managed);

// --- Client side: decode the wire response (ua) into the managed response. ---

CreateSubscriptionResponse ToManaged(
    const ua::CreateSubscriptionResponse& wire);
ModifySubscriptionResponse ToManaged(
    const ua::ModifySubscriptionResponse& wire);

// --- Publish / Republish. ---
//
// The managed NotificationMessage carries a std::vector<NotificationData>
// (a std::variant of DataChange/Event/StatusChange notifications); the
// generated one carries a std::vector<ExtensionObject>. Each notification is
// converted with To/FromExtensionObject, so a NotificationData that arrives in
// an unrecognized extension type is dropped rather than mis-decoded.

PublishRequest ToManaged(const ua::PublishRequest& wire);
RepublishRequest ToManaged(const ua::RepublishRequest& wire);
ua::PublishResponse ToWire(const PublishResponse& managed);
ua::RepublishResponse ToWire(const RepublishResponse& managed);

ua::PublishRequest ToWire(const PublishRequest& managed);
ua::RepublishRequest ToWire(const RepublishRequest& managed);
PublishResponse ToManaged(const ua::PublishResponse& wire);
RepublishResponse ToManaged(const ua::RepublishResponse& wire);

}  // namespace opcua::subscription_conversion
