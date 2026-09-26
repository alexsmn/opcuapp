#include "opcua/types/qualifier.h"

namespace opcua {
std::string ToString(opcua::Qualifier qualifier) {
  std::string text;
  if (qualifier.bad())
    text += 'B';
  if (qualifier.backup())
    text += 'R';
  if (qualifier.offline())
    text += 'O';
  if (qualifier.manual())
    text += 'M';
  if (qualifier.misconfigured())
    text += 'C';
  if (qualifier.simulated())
    text += 'E';
  if (qualifier.sporadic())
    text += 'S';
  if (qualifier.stale())
    text += 'T';
  if (qualifier.failed())
    text += 'F';
  return text;
}

// Spells each set flag as its enum name. opcuapp carries no operator-facing
// wording, so these are invariant identifiers rather than display text; an
// application that shows quality to an operator words the flags itself.
std::u16string ToString16(opcua::Qualifier qualifier) {
  std::u16string text;
  if (qualifier.bad())
    text += u"BAD ";
  if (qualifier.backup())
    text += u"BACKUP ";
  if (qualifier.offline())
    text += u"OFFLINE ";
  if (qualifier.manual())
    text += u"MANUAL ";
  if (qualifier.misconfigured())
    text += u"MISCONFIGURED ";
  if (qualifier.simulated())
    text += u"SIMULATED ";
  if (qualifier.sporadic())
    text += u"SPORADIC ";
  if (qualifier.stale())
    text += u"STALE ";
  if (qualifier.failed())
    text += u"FAILED ";
  return text;
}
}  // namespace opcua
