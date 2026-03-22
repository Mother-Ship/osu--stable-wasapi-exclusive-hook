#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <cstdio>
#include <string>

#include "fs_utils.h"

namespace wasapi_common
{
inline std::string FormatWin32Error(DWORD error)
{
    LPSTR message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = ::FormatMessageA(flags, nullptr, error, 0, reinterpret_cast<LPSTR>(&message), 0, nullptr);
    std::string text;
    if (length != 0 && message != nullptr)
    {
        text.assign(message, message + length);
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
            text.pop_back();
    }
    else
    {
        char buffer[64] = {};
        std::snprintf(buffer, sizeof(buffer), u8"Win32 \u9519\u8bef %lu", static_cast<unsigned long>(error));
        text = buffer;
    }

    if (message != nullptr)
        ::LocalFree(message);

    return text;
}

inline void AppendLogLine(const std::wstring& path, const std::string& line)
{
    const std::wstring directory = ParentDirectory(path);
    EnsureDirectoryExists(directory);

    HANDLE file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    SYSTEMTIME local_time = {};
    ::GetLocalTime(&local_time);

    char prefix[64] = {};
    std::snprintf(prefix, sizeof(prefix), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
                  local_time.wYear, local_time.wMonth, local_time.wDay, local_time.wHour,
                  local_time.wMinute, local_time.wSecond, local_time.wMilliseconds);

    DWORD written = 0;
    const std::string payload = std::string(prefix) + line + "\r\n";
    ::WriteFile(file, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);
    ::CloseHandle(file);
}
} // namespace wasapi_common
