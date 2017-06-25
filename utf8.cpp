#include "stdafx.h"
#include "bff.h"

std::string utf8(const std::wstring & s)
{
	int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.length(), nullptr, 0, nullptr, nullptr);
	char * buf = (char *)alloca(len + 2);
	memset(buf, 0, len + 2);
	WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.length(), buf, len + 1, nullptr, nullptr);
	return std::string(buf);
}

std::wstring utf8(const std::string & s)
{
	int len = MultiByteToWideChar(CP_UTF8, MB_PRECOMPOSED, s.c_str(), (int)s.length(), nullptr, 0);
	wchar_t * buf = (wchar_t *)alloca(sizeof(wchar_t) * (len + 2));
	memset(buf, 0, sizeof(wchar_t) * (len + 2));
	MultiByteToWideChar(CP_UTF8, MB_PRECOMPOSED, s.c_str(), (int)s.length(), buf, len + 1);
	return std::wstring(buf);
}

std::string ansi(const std::wstring & s)
{
	int len = WideCharToMultiByte(CP_THREAD_ACP, 0, s.c_str(), (int)s.length(), nullptr, 0, nullptr, nullptr);
	char * buf = (char *)alloca(len + 2);
	memset(buf, 0, len + 2);
	WideCharToMultiByte(CP_THREAD_ACP, 0, s.c_str(), (int)s.length(), buf, len + 1, nullptr, nullptr);
	return std::string(buf);
}

std::wstring ansi(const std::string & s)
{
	int len = MultiByteToWideChar(CP_THREAD_ACP, MB_PRECOMPOSED, s.c_str(), (int)s.length(), nullptr, 0);
	wchar_t * buf = (wchar_t *)alloca(sizeof(wchar_t) * (len + 2));
	memset(buf, 0, sizeof(wchar_t) * (len + 2));
	MultiByteToWideChar(CP_THREAD_ACP, MB_PRECOMPOSED, s.c_str(), (int)s.length(), buf, len + 1);
	return std::wstring(buf);
}
