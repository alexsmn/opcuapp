#include "opcua/types/status.h"

#include "opcua/base/utf_convert.h"

namespace opcua {

// static
Status Status::FromFullCode(unsigned full_code) {
  Status result(StatusCode::Bad);
  result.full_code_ = full_code;
  return result;
}

namespace {

struct Entry {
  opcua::StatusCode code;
  const char* error_string;
  const wchar_t* localized_error_string;
};

const Entry kEntries[] = {
    {opcua::StatusCode::Good, "Good", L"Операция выполнена успешно"},
    {opcua::StatusCode::Good_Pending, "Good_Pending", L"Операция выполняется"},
    {opcua::StatusCode::Uncertain_StateWasNotChanged,
     "Uncertain_StateWasNotChanged", L"Блокировка не была изменена"},
    {opcua::StatusCode::Bad, "Bad", L"Ошибка"},
    {opcua::StatusCode::Bad_WrongLoginCredentials, "Bad_WrongLoginCredentials",
     L"Неверное имя пользователя или пароль"},
    {opcua::StatusCode::Bad_UserIsAlreadyLoggedOn, "Bad_UserIsAlreadyLoggedOn",
     L"Сессия данного пользователя уже установлена"},
    {opcua::StatusCode::Bad_UnsupportedProtocolVersion,
     "Bad_UnsupportedProtocolVersion", L"Версия протокола не поддерживается"},
    {opcua::StatusCode::Bad_ObjectIsBusy, "Bad_ObjectIsBusy",
     L"В данный момент выполняется другая команда"},
    {opcua::StatusCode::Bad_WrongNodeId, "Bad_WrongNodeId",
     L"Неправильный идентификатор узла"},
    {opcua::StatusCode::Bad_WrongDeviceId, "Bad_WrongDeviceId",
     L"Неправильный идентификатор устройства"},
    {opcua::StatusCode::Bad_Disconnected, "Bad_Disconnected",
     L"Соединение не установлено"},
    {opcua::StatusCode::Bad_SessionForcedLogoff, "Bad_SessionForcedLogoff",
     L"Сессия разорвана из-за повторного подключения данного пользователя"},
    {opcua::StatusCode::Bad_Timeout, "Bad_Timeout",
     L"Операция прервана по истечении времени ожидания"},
    {opcua::StatusCode::Bad_CantDeleteDependentNode,
     "Bad_CantDeleteDependentNode",
     L"Невозможно удалить объект из-за наличия зависимых объектов"},
    {opcua::StatusCode::Bad_ServerWasShutDown, "Bad_ServerWasShutDown",
     L"Сессия разорвана из-за остановки сервера"},
    {opcua::StatusCode::Bad_WrongMethodId, "Bad_WrongMethodId",
     L"Команда не поддерживается данным объектом"},
    {opcua::StatusCode::Bad_CantDeleteOwnUser, "Bad_CantDeleteOwnUser",
     L"Невозможно удалить пользователя из открытой им сессии"},
    {opcua::StatusCode::Bad_DuplicateNodeId, "Bad_DuplicateNodeId",
     L"Объект с таким идентификатором уже существует"},
    {opcua::StatusCode::Bad_UnsupportedFileVersion,
     "Bad_UnsupportedFileVersion", L"Версия файла не поддерживается"},
    {opcua::StatusCode::Bad_WrongTypeId, "Bad_WrongTypeId",
     L"Неправильный тип объекта"},
    {opcua::StatusCode::Bad_WrongParentId, "Bad_WrongParentId",
     L"Неправильный идентификатор родительского объекта"},
    {opcua::StatusCode::Bad_SessionIsLoggedOff, "Bad_SessionIsLoggedOff",
     L"Авторизация не выполнена"},
    {opcua::StatusCode::Bad_WrongSubscriptionId, "Bad_WrongSubscriptionId",
     L"Неправильный номер подписки"},
    {opcua::StatusCode::Bad_WrongIndex, "Bad_WrongIndex",
     L"Неправильный индекс"},
    {opcua::StatusCode::Bad_Iec60870UnknownType, "Bad_IecUnknownType",
     L"Неправильный тип ASDU протокола МЭК-60870"},
    {opcua::StatusCode::Bad_Iec60870UnknownCot, "Bad_IecUnknownCot",
     L"Неправильная причина передачи протокола МЭК-60870"},
    {opcua::StatusCode::Bad_Iec60870UnknownDevice, "Bad_IecUnknownDevice",
     L"Неправильный адрес устройства протокола МЭК-60870"},
    {opcua::StatusCode::Bad_Iec60870UnknownAddress, "Bad_IecUnknownAddress",
     L"Неправильный адрес объекта протокола МЭК-60870"},
    {opcua::StatusCode::Bad_Iec60870UnknownError, "Bad_IecUnknownError",
     L"Ошибка протокола МЭК-60870"},
    {opcua::StatusCode::Bad_WrongCallArguments, "Bad_WrongCallArguments",
     L"Неправильные аргументы команды"},
    {opcua::StatusCode::Bad_CantParseString, "Bad_CantParseString",
     L"Невозможно преобразовать строку в значение данного типа"},
    {opcua::StatusCode::Bad_TooLongString, "Bad_TooLongString",
     L"Слишком длинная строка"},
    {opcua::StatusCode::Bad_WrongPropertyId, "Bad_WrongPropertyId",
     L"Неправильный атрибут объекта"},
    {opcua::StatusCode::Bad_WrongReferenceId, "Bad_WrongReferenceId",
     L"Неправильный тип ссылки"},
    {opcua::StatusCode::Bad_WrongNodeClass, "Bad_WrongNodeClass",
     L"Неправильный класс узла"},
    {opcua::StatusCode::Bad_Iec61850Error, "Bad_Iec61850Error",
     L"Ошибка протокола МЭК-61850"},
    {opcua::StatusCode::Bad_NothingToDo, "Bad_NothingToDo", L"Запрос пуст"},
    {opcua::StatusCode::Bad_BrowseNameInvalid, "Bad_BrowseNameInvalid",
     L"Имя не найдено"},
    {opcua::StatusCode::Bad_MonitoredItemIdInvalid,
     "Bad_MonitoredItemIdInvalid", L"Неправильный номер элемента мониторинга"},
    {opcua::StatusCode::Bad_MessageNotAvailable, "Bad_MessageNotAvailable",
     L"Запрошенное сообщение больше недоступно"},
    {opcua::StatusCode::Bad_ApplicationSignatureInvalid,
     "Bad_ApplicationSignatureInvalid", L"Неверная подпись приложения клиента"},
    {opcua::StatusCode::Bad_TooManyOperations, "Bad_TooManyOperations",
     L"Слишком много операций в запросе"},
    {opcua::StatusCode::Bad_TooManyMonitoredItems, "Bad_TooManyMonitoredItems",
     L"Слишком много элементов мониторинга в запросе"},
    {opcua::StatusCode::Bad_SequenceNumberUnknown, "Bad_SequenceNumberUnknown",
     L"Неизвестный порядковый номер сообщения"},
    {opcua::StatusCode::Bad_NoContinuationPoints, "Bad_NoContinuationPoints",
     L"Исчерпан лимит точек продолжения просмотра"},
    {opcua::StatusCode::Bad_TimestampsToReturnInvalid,
     "Bad_TimestampsToReturnInvalid",
     L"Неправильное значение TimestampsToReturn"},
    {opcua::StatusCode::Bad_ViewIdUnknown, "Bad_ViewIdUnknown",
     L"Неизвестный идентификатор представления"},
    {opcua::StatusCode::Bad_HistoryOperationInvalid,
     "Bad_HistoryOperationInvalid", L"Недопустимые параметры запроса истории"},
    {opcua::StatusCode::Bad_NoSubscription, "Bad_NoSubscription",
     L"Для сессии нет подписок"},
    {opcua::StatusCode::Bad_ServiceUnsupported, "Bad_ServiceUnsupported",
     L"Сервис не поддерживается"},
};

const Entry* FindEntry(opcua::StatusCode status_code) {
  for (auto& entry : kEntries) {
    if (entry.code == status_code)
      return &entry;
  }
  return nullptr;
}

}  // namespace

const char* ToCString(opcua::StatusCode status_code) {
  if (auto* entry = FindEntry(status_code))
    return entry->error_string;

  return IsGood(status_code) ? "OK" : "Error";
}

std::string ToString(opcua::StatusCode status_code) {
  return std::string{ToCString(status_code)};
}

std::u16string ToString16(opcua::StatusCode status_code) {
  if (auto* entry = FindEntry(status_code))
    return UtfConvert<char16_t>(entry->localized_error_string);

  return IsGood(status_code) ? u"Операция выполнена успешно" : u"Ошибка";
}

std::string ToString(const opcua::Status& status) {
  return ToString(status.code());
}

std::u16string ToString16(const opcua::Status& status) {
  return ToString16(status.code());
}
}  // namespace opcua
