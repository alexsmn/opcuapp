#pragma once

#include "opcua/message.h"
#include "opcua/ua/ua_types.h"

// Converts between the generated, spec-conformant OPC UA discovery wire types
// (`ua::FindServersRequest` & co.) and the hand-written discovery vocabulary in
// opcua/message.h (`opcua::FindServersRequest` & co.).
//
// Like the session types, the hand-written discovery types double as server
// configuration (the runtime's advertised endpoints and the registered-server
// registry are `opcua::EndpointDescription` / `opcua::ApplicationDescription`),
// and they are the client's endpoint-selection vocabulary. So the discovery
// cutover is codec-internal — the wire encoding is derived from the schema
// while the runtime, handler, and client are untouched — and these helpers are
// the seam both codecs share.
//
// Only the message *body* fields are mapped; the request/response header is
// reconciled separately by the codec's header seam.
namespace opcua::discovery_conversion {

// --- Server side: decode wire request (ua) into the managed request. ---

FindServersRequest ToManaged(const ua::FindServersRequest& wire);
GetEndpointsRequest ToManaged(const ua::GetEndpointsRequest& wire);
RegisterServerRequest ToManaged(const ua::RegisterServerRequest& wire);
// RegisterServer2's discoveryConfiguration is an array of ExtensionObjects.
// Each entry that decodes as an MdnsDiscoveryConfiguration (binary or inline
// JSON body) becomes a populated optional; any other extension type becomes
// nullopt so the handler can answer Bad_NotSupported for it by index (OPC UA
// Part 4 §5.4.6.2).
RegisterServer2Request ToManaged(const ua::RegisterServer2Request& wire);

// --- Server side: encode the managed response into the wire response (ua). ---

ua::FindServersResponse ToWire(const FindServersResponse& managed);
ua::GetEndpointsResponse ToWire(const GetEndpointsResponse& managed);
ua::RegisterServerResponse ToWire(const RegisterServerResponse& managed);
ua::RegisterServer2Response ToWire(const RegisterServer2Response& managed);

// --- Client side: encode the managed request into the wire request (ua). ---

ua::FindServersRequest ToWire(const FindServersRequest& managed);
ua::GetEndpointsRequest ToWire(const GetEndpointsRequest& managed);
ua::RegisterServerRequest ToWire(const RegisterServerRequest& managed);
ua::RegisterServer2Request ToWire(const RegisterServer2Request& managed);

// --- Client side: decode the wire response (ua) into the managed response. ---

FindServersResponse ToManaged(const ua::FindServersResponse& wire);
GetEndpointsResponse ToManaged(const ua::GetEndpointsResponse& wire);
RegisterServerResponse ToManaged(const ua::RegisterServerResponse& wire);
RegisterServer2Response ToManaged(const ua::RegisterServer2Response& wire);

}  // namespace opcua::discovery_conversion
