#pragma once

#include "opcua/session/server_session_manager.h"
#include "opcua/ua/ua_types.h"

#include <optional>

// Converts between the generated, spec-conformant OPC UA session wire types
// (`ua::CreateSessionRequest` & co., encoded/decoded by the schema-derived
// binary and JSON codecs) and the session manager's hand-written vocabulary
// (`opcua::CreateSessionRequest` & co. in server_session_manager.h).
//
// The manager types are kept deliberately: the server runtime injects
// connection-only fields (channel binding, peer) into them before handing them
// to the security-critical ServerSessionManager, and the response types carry
// runtime-only fields (ServiceContext, AuthenticationResult) that never travel
// on the wire. So the session cutover is codec-internal — the wire encoding is
// derived from the schema while the manager and runtime are untouched — and
// these helpers are the seam both codecs share.
//
// The wire request header (authentication token, request handle, traceparent)
// is reconciled separately by the codec's header seam; these helpers only map
// the message *body* fields. On the request-decode side the manager's
// authentication_token is read from the wire request header here so the two
// codecs agree.
namespace opcua::session_conversion {

// --- Server side: decode wire request (ua) into the managed request. ---

CreateSessionRequest ToManaged(const ua::CreateSessionRequest& wire);

// Decodes the UserIdentityToken ExtensionObject (Anonymous / UserName, in
// either the binary or the JSON body form) and routes an encrypted vs.
// cleartext password. Returns nullopt when the authentication token is not the
// expected numeric form or the identity token is missing/unsupported — the
// codec turns that into a decode failure, exactly as the hand-written decoder
// did (OPC UA Part 4 §5.6.3, §7.36).
std::optional<ActivateSessionRequest> ToManaged(
    const ua::ActivateSessionRequest& wire);

CloseSessionRequest ToManaged(const ua::CloseSessionRequest& wire);

// --- Server side: encode the managed response into the wire response (ua). ---
// Only the wire-visible fields are mapped; the response header service result
// is filled from the managed status. The request handle is injected by the
// codec's header seam.

ua::CreateSessionResponse ToWire(const CreateSessionResponse& managed);
ua::ActivateSessionResponse ToWire(const ActivateSessionResponse& managed);
ua::CloseSessionResponse ToWire(const CloseSessionResponse& managed);

// --- Client side: encode the managed request into the wire request (ua). ---
// The request header is filled by the codec's header seam.

ua::CreateSessionRequest ToWire(const CreateSessionRequest& managed);
ua::ActivateSessionRequest ToWire(const ActivateSessionRequest& managed);
ua::CloseSessionRequest ToWire(const CloseSessionRequest& managed);

// --- Client side: decode the wire response (ua) into the managed response. ---

CreateSessionResponse ToManaged(const ua::CreateSessionResponse& wire);
ActivateSessionResponse ToManaged(const ua::ActivateSessionResponse& wire);
CloseSessionResponse ToManaged(const ua::CloseSessionResponse& wire);

}  // namespace opcua::session_conversion
