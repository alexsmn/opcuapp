#include "opcua/scada/qualifier.h"


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

std::u16string ToString16(opcua::Qualifier qualifier) {
  std::u16string text;
  if (qualifier.bad())
    text += u"Недост ";
  if (qualifier.backup())
    text += u"Резерв ";
  if (qualifier.offline())
    text += u"НетСвязи ";
  if (qualifier.manual())
    text += u"Ручной ";
  if (qualifier.misconfigured())
    text += u"НеСконф ";
  if (qualifier.simulated())
    text += u"Эмулирован ";
  if (qualifier.sporadic())
    text += u"Спорадика ";
  if (qualifier.stale())
    text += u"Устарел ";
  if (qualifier.failed())
    text += u"Ошибка ";
  return text;
}
}  // namespace opcua (vendored)
