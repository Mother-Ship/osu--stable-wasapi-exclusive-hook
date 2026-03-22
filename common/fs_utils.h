#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <string>

namespace wasapi_common
{
inline std::wstring GetModuleFilePath(HMODULE module)
{
    wchar_t buffer[MAX_PATH] = {};
    DWORD length = ::GetModuleFileNameW(module, buffer, MAX_PATH);
    return std::wstring(buffer, buffer + length);
}

inline std::wstring ParentDirectory(const std::wstring& path)
{
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return L".";

    return path.substr(0, slash);
}

inline std::wstring JoinPath(const std::wstring& lhs, const std::wstring& rhs)
{
    if (lhs.empty())
        return rhs;

    if (lhs.back() == L'\\' || lhs.back() == L'/')
        return lhs + rhs;

    return lhs + L"\\" + rhs;
}

inline std::wstring NormalizeFullPath(const std::wstring& path)
{
    wchar_t buffer[MAX_PATH] = {};
    DWORD length = ::GetFullPathNameW(path.c_str(), MAX_PATH, buffer, nullptr);
    if (length == 0 || length >= MAX_PATH)
        return path;

    return std::wstring(buffer, buffer + length);
}

inline std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
        return std::string();

    const int required = ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1)
        return std::string();

    std::string converted(static_cast<std::size_t>(required - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, &converted[0], required, nullptr, nullptr);
    return converted;
}

inline std::string AnsiToUtf8(const char* value, UINT code_page = CP_ACP)
{
    if (value == nullptr || *value == '\0')
        return std::string();

    const int required_wide = ::MultiByteToWideChar(code_page, 0, value, -1, nullptr, 0);
    if (required_wide <= 1)
        return std::string();

    std::wstring wide(static_cast<std::size_t>(required_wide - 1), L'\0');
    ::MultiByteToWideChar(code_page, 0, value, -1, &wide[0], required_wide);
    return WideToUtf8(wide);
}

inline bool EnsureDirectoryExists(const std::wstring& path)
{
    if (path.empty() || path == L"." || path == L"..")
        return true;

    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    const std::wstring parent = ParentDirectory(path);
    if (!parent.empty() && parent != path)
    {
        if (!EnsureDirectoryExists(parent))
            return false;
    }

    if (::CreateDirectoryW(path.c_str(), nullptr))
        return true;

    return ::GetLastError() == ERROR_ALREADY_EXISTS;
}
} // namespace wasapi_common
