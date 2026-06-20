#pragma once

#include <string>

namespace opcua::base {
class Time;
}

enum TimeFormatFlags {
	TIME_FORMAT_DATE = 0x0100,
	TIME_FORMAT_TIME = 0x0200,
	TIME_FORMAT_MSEC = 0x0400,
	TIME_FORMAT_UTC  = 0x0800,

	TIME_FORMAT_DEFAULT	= TIME_FORMAT_DATE | TIME_FORMAT_TIME | TIME_FORMAT_MSEC,
};

std::string FormatTime(opcua::base::Time time, int flags = TIME_FORMAT_DEFAULT);
