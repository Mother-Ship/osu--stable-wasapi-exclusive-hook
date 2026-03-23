#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../common/fs_utils.h"
#include "../common/logging.h"

namespace
{
using namespace wasapi_common;

std::wstring g_log_path;

void CloseHandleIfValid(HANDLE handle)
{
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
        ::CloseHandle(handle);
}

void CloseProcessHandles(PROCESS_INFORMATION& process_info)
{
    CloseHandleIfValid(process_info.hThread);
    CloseHandleIfValid(process_info.hProcess);
    process_info.hThread = nullptr;
    process_info.hProcess = nullptr;
}

void FreeRemoteAllocation(HANDLE process, void* remote_buffer)
{
    if (remote_buffer != nullptr)
        ::VirtualFreeEx(process, remote_buffer, 0, MEM_RELEASE);
}

void Log(const std::string& line)
{
    if (!g_log_path.empty())
        AppendLogLine(g_log_path, line);
}

void LogLastError(const char* prefix)
{
    Log(std::string(prefix) + ": " + FormatWin32Error(::GetLastError()));
}

const char* DescribeInitExitCode(DWORD exit_code)
{
    switch (exit_code)
    {
    case ERROR_SUCCESS:
        return u8"\u6210\u529f";
    case ERROR_ALREADY_INITIALIZED:
        return u8"\u5df2\u521d\u59cb\u5316";
    case ERROR_MOD_NOT_FOUND:
        return u8"\u7f3a\u5c11\u6a21\u5757";
    case ERROR_PROC_NOT_FOUND:
        return u8"\u7f3a\u5c11\u5bfc\u51fa\u51fd\u6570";
    case ERROR_INVALID_FUNCTION:
        return u8"\u51fd\u6570\u4fee\u8865\u5931\u8d25";
    case ERROR_GEN_FAILURE:
        return u8"\u901a\u7528\u5931\u8d25";
    default:
        return u8"\u672a\u77e5\u9519\u8bef";
    }
}

std::wstring Quote(const std::wstring& value)
{
    return L"\"" + value + L"\"";
}

std::wstring QuoteCommandLineArg(const std::wstring& value)
{
    if (value.empty())
        return L"\"\"";

    bool needs_quotes = false;
    for (wchar_t ch : value)
    {
        if (ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\"')
        {
            needs_quotes = true;
            break;
        }
    }

    if (!needs_quotes)
        return value;

    std::wstring result;
    result.reserve(value.size() + 2);
    result.push_back(L'"');

    std::size_t backslash_count = 0;
    for (wchar_t ch : value)
    {
        if (ch == L'\\')
        {
            ++backslash_count;
            continue;
        }

        if (ch == L'"')
        {
            result.append(backslash_count * 2 + 1, L'\\');
            result.push_back(L'"');
            backslash_count = 0;
            continue;
        }

        if (backslash_count > 0)
        {
            result.append(backslash_count, L'\\');
            backslash_count = 0;
        }

        result.push_back(ch);
    }

    if (backslash_count > 0)
        result.append(backslash_count * 2, L'\\');

    result.push_back(L'"');
    return result;
}

bool IsX86PortableExecutable(const std::wstring& path)
{
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr)
    {
        CloseHandleIfValid(file);
        return false;
    }

    const void* view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr)
    {
        CloseHandleIfValid(mapping);
        CloseHandleIfValid(file);
        return false;
    }

    const auto* dos = static_cast<const IMAGE_DOS_HEADER*>(view);
    bool is_x86 = false;
    if (dos->e_magic == IMAGE_DOS_SIGNATURE)
    {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(static_cast<const std::uint8_t*>(view) + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE)
            is_x86 = (nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386);
    }

    ::UnmapViewOfFile(view);
    CloseHandleIfValid(mapping);
    CloseHandleIfValid(file);
    return is_x86;
}

bool ResolveHookExportRva(const std::wstring& hook_path, const char* export_name, std::uintptr_t& rva)
{
    HMODULE local_module = ::LoadLibraryExW(hook_path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (local_module == nullptr)
    {
        LogLastError(u8"\u52a0\u8f7d Hook \u6a21\u5757\u5931\u8d25");
        return false;
    }

    FARPROC export_address = ::GetProcAddress(local_module, export_name);
    if (export_address == nullptr && std::strcmp(export_name, "InitializeHookBridge") == 0)
        export_address = ::GetProcAddress(local_module, "_InitializeHookBridge@4");

    if (export_address == nullptr)
    {
        LogLastError(u8"\u83b7\u53d6\u521d\u59cb\u5316\u5bfc\u51fa\u51fd\u6570\u5931\u8d25");
        ::FreeLibrary(local_module);
        return false;
    }

    rva = reinterpret_cast<std::uintptr_t>(export_address) - reinterpret_cast<std::uintptr_t>(local_module);
    ::FreeLibrary(local_module);
    return true;
}

std::uintptr_t FindRemoteModuleBase(DWORD process_id, const wchar_t* module_name)
{
    for (int attempt = 0; attempt < 40; ++attempt)
    {
        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            Log(std::string(u8"\u521b\u5efa\u6a21\u5757\u5feb\u7167\u5931\u8d25: ") + FormatWin32Error(::GetLastError()));
            ::Sleep(25);
            continue;
        }

        MODULEENTRY32W entry = {};
        entry.dwSize = sizeof(entry);

        std::uintptr_t base = 0;
        if (::Module32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szModule, module_name) == 0)
                {
                    base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                    break;
                }
            } while (::Module32NextW(snapshot, &entry));
        }
        else
        {
            Log(std::string(u8"\u679a\u4e3e\u8fdc\u7a0b\u6a21\u5757\u5931\u8d25: ") + FormatWin32Error(::GetLastError()));
        }

        ::CloseHandle(snapshot);
        if (base != 0)
            return base;

        ::Sleep(25);
    }

    return 0;
}

bool RemoteLoadLibrary(HANDLE process, const std::wstring& dll_path, HMODULE& remote_module)
{
    const SIZE_T payload_size = (dll_path.size() + 1) * sizeof(wchar_t);
    void* remote_buffer = ::VirtualAllocEx(process, nullptr, payload_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote_buffer == nullptr)
    {
        LogLastError(u8"\u8fdc\u7a0b\u5206\u914d\u5185\u5b58\u5931\u8d25");
        return false;
    }

    if (!::WriteProcessMemory(process, remote_buffer, dll_path.c_str(), payload_size, nullptr))
    {
        LogLastError(u8"\u5199\u5165\u8fdc\u7a0b\u8fdb\u7a0b\u5931\u8d25");
        FreeRemoteAllocation(process, remote_buffer);
        return false;
    }

    const DWORD process_id = ::GetProcessId(process);
    const std::uintptr_t remote_kernel32 = FindRemoteModuleBase(process_id, L"KERNEL32.DLL");
    HMODULE local_kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    if (local_kernel32 == nullptr)
    {
        Log(u8"\u89e3\u6790\u672c\u5730 kernel32!LoadLibraryW \u5931\u8d25");
        FreeRemoteAllocation(process, remote_buffer);
        return false;
    }

    FARPROC local_load_library = ::GetProcAddress(local_kernel32, "LoadLibraryW");
    if (local_load_library == nullptr)
    {
        Log(u8"\u89e3\u6790\u672c\u5730 kernel32!LoadLibraryW \u5931\u8d25");
        FreeRemoteAllocation(process, remote_buffer);
        return false;
    }

    const std::uintptr_t load_library_rva =
        reinterpret_cast<std::uintptr_t>(local_load_library) - reinterpret_cast<std::uintptr_t>(local_kernel32);
    LPTHREAD_START_ROUTINE load_library = nullptr;
    if (remote_kernel32 != 0)
    {
        load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_kernel32 + load_library_rva);
        Log(u8"\u5df2\u89e3\u6790\u8fdc\u7a0b kernel32 \u57fa\u5740");
    }
    else
    {
        load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(local_load_library);
        Log(u8"\u672a\u83b7\u53d6\u5230\u8fdc\u7a0b kernel32 \u57fa\u5740, \u56de\u9000\u5230\u672c\u5730 LoadLibraryW \u5730\u5740");
    }

    HANDLE thread = ::CreateRemoteThread(process, nullptr, 0, load_library, remote_buffer, 0, nullptr);
    if (thread == nullptr)
    {
        LogLastError(u8"\u521b\u5efa\u8fdc\u7a0b LoadLibraryW \u7ebf\u7a0b\u5931\u8d25");
        FreeRemoteAllocation(process, remote_buffer);
        return false;
    }

    const DWORD wait_result = ::WaitForSingleObject(thread, 15000);
    DWORD exit_code = 0;
    if (wait_result != WAIT_OBJECT_0 || !::GetExitCodeThread(thread, &exit_code) || exit_code == 0)
    {
        Log(wait_result == WAIT_OBJECT_0 ? u8"\u8fdc\u7a0b LoadLibraryW \u8fd4\u56de\u4e86\u7a7a\u6a21\u5757\u53e5\u67c4"
                                         : u8"\u7b49\u5f85\u8fdc\u7a0b LoadLibraryW \u8d85\u65f6");
        CloseHandleIfValid(thread);
        FreeRemoteAllocation(process, remote_buffer);
        return false;
    }

    remote_module = reinterpret_cast<HMODULE>(static_cast<std::uintptr_t>(exit_code));
    CloseHandleIfValid(thread);
    FreeRemoteAllocation(process, remote_buffer);
    return true;
}

bool RemoteCall(HANDLE process, LPTHREAD_START_ROUTINE routine, void* parameter, DWORD& exit_code)
{
    HANDLE thread = ::CreateRemoteThread(process, nullptr, 0, routine, parameter, 0, nullptr);
    if (thread == nullptr)
    {
        LogLastError(u8"\u521b\u5efa\u8fdc\u7a0b\u521d\u59cb\u5316\u7ebf\u7a0b\u5931\u8d25");
        return false;
    }

    const DWORD wait_result = ::WaitForSingleObject(thread, 15000);
    const bool ok = (wait_result == WAIT_OBJECT_0) && ::GetExitCodeThread(thread, &exit_code);
    if (!ok)
        Log(wait_result == WAIT_OBJECT_0 ? u8"\u8bfb\u53d6\u8fdc\u7a0b\u521d\u59cb\u5316\u7ebf\u7a0b\u9000\u51fa\u7801\u5931\u8d25"
                                         : u8"\u7b49\u5f85 Hook \u521d\u59cb\u5316\u8d85\u65f6");

    CloseHandleIfValid(thread);
    return ok;
}

bool ParseArguments(int argc, wchar_t** argv, std::wstring& game_path, std::vector<std::wstring>& game_args)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring current = argv[i];
        if (current == L"--game" && i + 1 < argc)
        {
            game_path = NormalizeFullPath(argv[++i]);
            continue;
        }

        if (current == L"--")
        {
            for (int j = i + 1; j < argc; ++j)
                game_args.emplace_back(argv[j]);
            break;
        }

        if (current == L"-h" || current == L"--help")
            return false;

        game_args.emplace_back(current);
    }

    return !game_path.empty();
}

void PrintUsage()
{
    std::fwprintf(stderr, L"\u7528\u6cd5: launcher.exe --game <osu!.exe \u8def\u5f84> [args...]\n");
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    const std::wstring launcher_dir = ParentDirectory(GetModuleFilePath(nullptr));
    const std::wstring root_dir = ParentDirectory(launcher_dir);
    const std::wstring hook_log_path = JoinPath(JoinPath(root_dir, L"logs"), L"hook.log");
    g_log_path = JoinPath(JoinPath(root_dir, L"logs"), L"launcher.log");

    std::wstring game_path;
    std::vector<std::wstring> game_args;
    if (!ParseArguments(argc, argv, game_path, game_args))
    {
        PrintUsage();
        Log(u8"\u547d\u4ee4\u884c\u53c2\u6570\u65e0\u6548");
        return 1;
    }

    const std::wstring hook_path = JoinPath(launcher_dir, L"osu_audio_hook.dll");
    if (::GetFileAttributesW(hook_path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        Log(std::string(u8"\u672a\u627e\u5230 Hook DLL: ") + WideToUtf8(hook_path));
        std::fwprintf(stderr, L"\u7f3a\u5c11 Hook DLL: %ls\n", hook_path.c_str());
        return 1;
    }

    if (!IsX86PortableExecutable(game_path))
    {
        Log(std::string(u8"\u76ee\u6807\u7a0b\u5e8f\u4e0d\u662f 32 \u4f4d PE \u53ef\u6267\u884c\u6587\u4ef6: ") + WideToUtf8(game_path));
        std::fwprintf(stderr, L"\u76ee\u6807\u7a0b\u5e8f\u5fc5\u987b\u662f 32 \u4f4d Windows \u53ef\u6267\u884c\u6587\u4ef6.\n");
        return 1;
    }

    std::uintptr_t init_rva = 0;
    if (!ResolveHookExportRva(hook_path, "InitializeHookBridge", init_rva))
    {
        std::fwprintf(stderr, L"\u89e3\u6790 Hook \u521d\u59cb\u5316\u51fd\u6570\u5931\u8d25.\n");
        return 1;
    }

    STARTUPINFOW startup_info = {};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info = {};

    std::wstring command_line = QuoteCommandLineArg(game_path);
    for (const auto& arg : game_args)
    {
        command_line.push_back(L' ');
        command_line += QuoteCommandLineArg(arg);
    }
    const std::wstring working_directory = ParentDirectory(game_path);
    if (!::CreateProcessW(game_path.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                          CREATE_SUSPENDED, nullptr, working_directory.c_str(), &startup_info, &process_info))
    {
        LogLastError(u8"\u521b\u5efa\u76ee\u6807\u8fdb\u7a0b\u5931\u8d25");
        std::fwprintf(stderr, L"\u542f\u52a8\u6e38\u620f\u5931\u8d25.\n");
        return 1;
    }

    Log(std::string(u8"\u5df2\u6302\u8d77\u542f\u52a8\u76ee\u6807\u8fdb\u7a0b: ") + WideToUtf8(game_path));

    HMODULE remote_module = nullptr;
    DWORD init_exit_code = ERROR_GEN_FAILURE;
    bool ok = RemoteLoadLibrary(process_info.hProcess, hook_path, remote_module);
    if (ok)
    {
        auto* remote_init = reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<std::uintptr_t>(remote_module) + init_rva);
        ok = RemoteCall(process_info.hProcess, remote_init, nullptr, init_exit_code) && init_exit_code == ERROR_SUCCESS;
        if (!ok)
            Log(std::string(u8"Hook \u521d\u59cb\u5316\u5931\u8d25, \u9000\u51fa\u7801=") + std::to_string(init_exit_code));
    }

    if (!ok)
    {
        ::TerminateProcess(process_info.hProcess, 1);
        CloseProcessHandles(process_info);
        Log(std::string(u8"\u521d\u59cb\u5316\u97f3\u9891 Hook \u5931\u8d25, \u9000\u51fa\u7801=") + std::to_string(init_exit_code) +
            " (" + DescribeInitExitCode(init_exit_code) + ")");
        std::fwprintf(stderr, L"\u521d\u59cb\u5316\u97f3\u9891 Hook \u5931\u8d25. exit_code=%lu (%hs)\n",
                      static_cast<unsigned long>(init_exit_code), DescribeInitExitCode(init_exit_code));
        std::fwprintf(stderr, L"\u8bf7\u67e5\u770b\u65e5\u5fd7:\n  %ls\n  %ls\n",
                      g_log_path.c_str(), hook_log_path.c_str());
        return 1;
    }

    ::ResumeThread(process_info.hThread);
    Log(u8"\u5df2\u6062\u590d\u76ee\u6807\u8fdb\u7a0b\u8fd0\u884c");

    CloseProcessHandles(process_info);
    return 0;
}
