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

// --- CreateMonitoredItems / ModifyMonitoredItems. ---
//
// The monitored-item filter is the subtle part. The managed
// MonitoringParameters carries an optional MonitoringFilter (a
// DataChangeFilter, or an EventFilter as an opaque JSON blob); the generated
// one carries an ExtensionObject. The DataChangeFilter maps to the typed
// ua::DataChangeFilter; the EventFilter JSON blob is reshaped into a conformant
// ua::EventFilter (SimpleAttributeOperand select clauses + a ContentFilter
// where-clause), and back. The managed filter_result (an opaque JSON blob, in
// practice always empty) rides the wire as an ExtensionObject.

CreateMonitoredItemsRequest ToManaged(
    const ua::CreateMonitoredItemsRequest& wire);
ModifyMonitoredItemsRequest ToManaged(
    const ua::ModifyMonitoredItemsRequest& wire);
ua::CreateMonitoredItemsResponse ToWire(
    const CreateMonitoredItemsResponse& managed);
ua::ModifyMonitoredItemsResponse ToWire(
    const ModifyMonitoredItemsResponse& managed);

ua::CreateMonitoredItemsRequest ToWire(
    const CreateMonitoredItemsRequest& managed);
ua::ModifyMonitoredItemsRequest ToWire(
    const ModifyMonitoredItemsRequest& managed);
CreateMonitoredItemsResponse ToManaged(
    const ua::CreateMonitoredItemsResponse& wire);
ModifyMonitoredItemsResponse ToManaged(
    const ua::ModifyMonitoredItemsResponse& wire);

}  // namespace opcua::subscription_conversion
