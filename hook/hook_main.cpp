#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <climits>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/bass_api.h"
#include "../common/fs_utils.h"
#include "../common/logging.h"

using namespace wasapi_bass;
using namespace wasapi_common;

namespace
{
struct TrackState
{
    bool attached = false;
    DWORD state = BASS_ACTIVE_STOPPED;
};

struct SampleBacking
{
    std::mutex mutex;
    std::vector<float> pcm;
    std::size_t cursor_samples = 0;
    DWORD channels = 2;
    bool loop = false;
};

struct SampleChannelState
{
    DWORD logical_handle = 0;
    DWORD sample_handle = 0;
    DWORD backing_stream = 0;
    bool attached = false;
    DWORD state = BASS_ACTIVE_STOPPED;
    float volume = 1.0f;
    float pan = 0.0f;
    float frequency = 0.0f;
    SampleBacking* backing = nullptr;
};

struct OriginalBassApi
{
    BASS_Init_Fn BASS_Init = nullptr;
    BASS_Free_Fn BASS_Free = nullptr;
    BASS_ErrorGetCode_Fn BASS_ErrorGetCode = nullptr;
    BASS_GetDevice_Fn BASS_GetDevice = nullptr;
    BASS_GetDeviceInfo_Fn BASS_GetDeviceInfo = nullptr;
    BASS_SetDevice_Fn BASS_SetDevice = nullptr;
    BASS_ChannelPlay_Fn BASS_ChannelPlay = nullptr;
    BASS_ChannelPause_Fn BASS_ChannelPause = nullptr;
    BASS_ChannelStop_Fn BASS_ChannelStop = nullptr;
    BASS_ChannelIsActive_Fn BASS_ChannelIsActive = nullptr;
    BASS_ChannelSetAttribute_Fn BASS_ChannelSetAttribute = nullptr;
    BASS_ChannelGetAttribute_Fn BASS_ChannelGetAttribute = nullptr;
    BASS_ChannelGetPosition_Fn BASS_ChannelGetPosition = nullptr;
    BASS_ChannelSetPosition_Fn BASS_ChannelSetPosition = nullptr;
    BASS_ChannelGetLength_Fn BASS_ChannelGetLength = nullptr;
    BASS_ChannelGetLevel_Fn BASS_ChannelGetLevel = nullptr;
    BASS_ChannelGetData_Fn BASS_ChannelGetData = nullptr;
    BASS_StreamCreate_Fn BASS_StreamCreate = nullptr;
    BASS_StreamCreateFileUser_Fn BASS_StreamCreateFileUser = nullptr;
    BASS_StreamFree_Fn BASS_StreamFree = nullptr;
    BASS_SampleGetChannel_Fn BASS_SampleGetChannel = nullptr;
    BASS_SampleGetInfo_Fn BASS_SampleGetInfo = nullptr;
    BASS_SampleGetData_Fn BASS_SampleGetData = nullptr;
    BASS_SampleFree_Fn BASS_SampleFree = nullptr;

    BASS_FX_TempoCreate_Fn BASS_FX_TempoCreate = nullptr;
    BASS_FX_ReverseCreate_Fn BASS_FX_ReverseCreate = nullptr;

    BASS_Mixer_StreamCreate_Fn BASS_Mixer_StreamCreate = nullptr;
    BASS_Mixer_StreamAddChannel_Fn BASS_Mixer_StreamAddChannel = nullptr;
    BASS_Mixer_ChannelRemove_Fn BASS_Mixer_ChannelRemove = nullptr;
    BASS_Mixer_ChannelFlags_Fn BASS_Mixer_ChannelFlags = nullptr;
    BASS_Mixer_ChannelSetPosition_Fn BASS_Mixer_ChannelSetPosition = nullptr;
    BASS_Mixer_ChannelGetPosition_Fn BASS_Mixer_ChannelGetPosition = nullptr;
    BASS_Mixer_ChannelGetLevel_Fn BASS_Mixer_ChannelGetLevel = nullptr;
    BASS_Mixer_ChannelGetData_Fn BASS_Mixer_ChannelGetData = nullptr;

    BASS_WASAPI_Init_Fn BASS_WASAPI_Init = nullptr;
    BASS_WASAPI_Free_Fn BASS_WASAPI_Free = nullptr;
    BASS_WASAPI_Start_Fn BASS_WASAPI_Start = nullptr;
    BASS_WASAPI_Stop_Fn BASS_WASAPI_Stop = nullptr;
    BASS_WASAPI_GetInfo_Fn BASS_WASAPI_GetInfo = nullptr;
    BASS_WASAPI_GetDeviceInfo_Fn BASS_WASAPI_GetDeviceInfo = nullptr;
};

struct RuntimeState
{
    HMODULE self = nullptr;
    HMODULE bass_module = nullptr;
    HMODULE bass_fx_module = nullptr;
    HMODULE bassmix_module = nullptr;
    HMODULE basswasapi_module = nullptr;

    std::wstring runtime_dir;
    std::wstring root_dir;
    std::wstring game_dir;
    std::wstring hook_log_path;

    std::recursive_mutex mutex;
    std::atomic<DWORD> next_logical_handle { 0x70000000u };

    bool initialized = false;
    bool bass_initialized = false;
    int active_bass_device = -1;
    int active_wasapi_device = -1;
    LONG block_next_fallback = 0;
    int last_unmapped_device = INT_MIN;
    DWORD last_unmapped_tick = 0;
    int last_wasapi_failure_device = INT_MIN;
    DWORD last_wasapi_failure_tick = 0;
    DWORD master_mixer = 0;

    std::unordered_map<DWORD, TrackState> tracks;
    std::unordered_map<DWORD, SampleChannelState> sample_channels;
};

struct ExportBinding
{
    HMODULE module;
    const char* name;
    FARPROC* target;
};

struct ExportPatch
{
    HMODULE module;
    const char* name;
    void* replacement;
};

OriginalBassApi g_api;
RuntimeState g_state;

void Log(const std::string& line)
{
    if (!g_state.hook_log_path.empty())
        AppendLogLine(g_state.hook_log_path, line);
}

std::string FormatMilliseconds(float seconds)
{
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.3fms", static_cast<double>(seconds * 1000.0f));
    return buffer;
}

template <typename T>
ExportBinding BindExport(HMODULE module, const char* name, T& target)
{
    return ExportBinding { module, name, reinterpret_cast<FARPROC*>(&target) };
}

bool ResolveExports(const ExportBinding* bindings, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        FARPROC export_proc = ::GetProcAddress(bindings[i].module, bindings[i].name);
        *bindings[i].target = export_proc;
        if (export_proc == nullptr)
        {
            Log(std::string(u8"\u7f3a\u5c11\u5bfc\u51fa\u7b26\u53f7: ") + bindings[i].name);
            return false;
        }
    }

    return true;
}

bool PatchNamedExport(HMODULE module, const char* export_name, void* replacement)
{
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        Log(std::string(u8"\u4fee\u8865\u5bfc\u51fa\u5931\u8d25, DOS \u5934\u65e0\u6548: ") + export_name);
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        Log(std::string(u8"\u4fee\u8865\u5bfc\u51fa\u5931\u8d25, NT \u5934\u65e0\u6548: ") + export_name);
        return false;
    }

    const IMAGE_DATA_DIRECTORY& export_directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (export_directory.VirtualAddress == 0 || export_directory.Size == 0)
    {
        Log(std::string(u8"\u4fee\u8865\u5bfc\u51fa\u5931\u8d25, \u5bfc\u51fa\u8868\u4e0d\u5b58\u5728: ") + export_name);
        return false;
    }

    auto* exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + export_directory.VirtualAddress);
    auto* names = reinterpret_cast<DWORD*>(base + exports->AddressOfNames);
    auto* ordinals = reinterpret_cast<WORD*>(base + exports->AddressOfNameOrdinals);
    auto* functions = reinterpret_cast<DWORD*>(base + exports->AddressOfFunctions);

    for (DWORD i = 0; i < exports->NumberOfNames; ++i)
    {
        const char* current = reinterpret_cast<const char*>(base + names[i]);
        if (std::strcmp(current, export_name) != 0)
            continue;

        DWORD* function_rva = &functions[ordinals[i]];
        DWORD old_protection = 0;
        if (!::VirtualProtect(function_rva, sizeof(DWORD), PAGE_READWRITE, &old_protection))
        {
            Log(std::string(u8"\u4fee\u8865\u5bfc\u51fa\u65f6 VirtualProtect \u5931\u8d25: ") + export_name);
            return false;
        }

        const auto new_rva = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(replacement) -
                                                reinterpret_cast<std::uintptr_t>(base));
        *function_rva = new_rva;
        ::FlushInstructionCache(::GetCurrentProcess(), function_rva, sizeof(DWORD));
        DWORD unused = 0;
        ::VirtualProtect(function_rva, sizeof(DWORD), old_protection, &unused);
        return true;
    }

    Log(std::string(u8"\u4fee\u8865\u5bfc\u51fa\u5931\u8d25, \u672a\u627e\u5230\u76ee\u6807\u7b26\u53f7: ") + export_name);
    return false;
}

bool ApplyExportPatches(const ExportPatch* patches, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!PatchNamedExport(patches[i].module, patches[i].name, patches[i].replacement))
            return false;
    }

    return true;
}

void LoadRuntimePaths()
{
    g_state.runtime_dir = ParentDirectory(GetModuleFilePath(g_state.self));
    g_state.root_dir = ParentDirectory(g_state.runtime_dir);
    g_state.game_dir = ParentDirectory(GetModuleFilePath(nullptr));
    g_state.hook_log_path = JoinPath(JoinPath(g_state.root_dir, L"logs"), L"hook.log");
}

bool ShouldLogRateLimited(int& last_device, DWORD& last_tick, int device, DWORD interval_ms = 1000)
{
    const DWORD now = ::GetTickCount();
    if (last_device == device && (now - last_tick) < interval_ms)
        return false;

    last_device = device;
    last_tick = now;
    return true;
}

bool LoadBassDeviceInfo(int device, BASS_DEVICEINFO_NATIVE& info)
{
    std::memset(&info, 0, sizeof(info));
    return g_api.BASS_GetDeviceInfo(device, &info) != FALSE;
}

bool IsUsableBassDevice(const BASS_DEVICEINFO_NATIVE& info)
{
    return info.driver != nullptr && (info.flags & BASS_DEVICE_ENABLED) != 0;
}

int FindPreferredDefaultBassDevice(BASS_DEVICEINFO_NATIVE* info_out = nullptr)
{
    int first_enabled = -1;
    BASS_DEVICEINFO_NATIVE first_enabled_info = {};

    for (int i = 0;; ++i)
    {
        BASS_DEVICEINFO_NATIVE info = {};
        if (!LoadBassDeviceInfo(i, info))
            break;

        if (!IsUsableBassDevice(info))
            continue;

        if (first_enabled < 0)
        {
            first_enabled = i;
            first_enabled_info = info;
        }

        if ((info.flags & BASS_DEVICE_DEFAULT) != 0)
        {
            if (info_out != nullptr)
                *info_out = info;
            return i;
        }
    }

    if (info_out != nullptr && first_enabled >= 0)
        *info_out = first_enabled_info;

    return first_enabled;
}

bool LoadModules()
{
    const std::wstring bass_path = JoinPath(g_state.game_dir, L"bass.dll");
    const std::wstring bass_fx_path = JoinPath(g_state.game_dir, L"bass_fx.dll");
    const std::wstring bassmix_path = JoinPath(g_state.runtime_dir, L"bassmix.dll");
    const std::wstring basswasapi_path = JoinPath(g_state.runtime_dir, L"basswasapi.dll");

    g_state.bass_module = ::LoadLibraryW(bass_path.c_str());
    if (g_state.bass_module == nullptr)
        Log(std::string(u8"\u52a0\u8f7d\u6a21\u5757\u5931\u8d25: ") + WideToUtf8(bass_path) +
            std::string(u8", \u9519\u8bef: ") + FormatWin32Error(::GetLastError()));

    g_state.bass_fx_module = ::LoadLibraryW(bass_fx_path.c_str());
    if (g_state.bass_fx_module == nullptr)
        Log(std::string(u8"\u52a0\u8f7d\u6a21\u5757\u5931\u8d25: ") + WideToUtf8(bass_fx_path) +
            std::string(u8", \u9519\u8bef: ") + FormatWin32Error(::GetLastError()));

    g_state.bassmix_module = ::LoadLibraryW(bassmix_path.c_str());
    if (g_state.bassmix_module == nullptr)
        Log(std::string(u8"\u52a0\u8f7d\u6a21\u5757\u5931\u8d25: ") + WideToUtf8(bassmix_path) +
            std::string(u8", \u9519\u8bef: ") + FormatWin32Error(::GetLastError()));

    g_state.basswasapi_module = ::LoadLibraryW(basswasapi_path.c_str());
    if (g_state.basswasapi_module == nullptr)
        Log(std::string(u8"\u52a0\u8f7d\u6a21\u5757\u5931\u8d25: ") + WideToUtf8(basswasapi_path) +
            std::string(u8", \u9519\u8bef: ") + FormatWin32Error(::GetLastError()));

    if (g_state.bass_module == nullptr || g_state.bass_fx_module == nullptr ||
        g_state.bassmix_module == nullptr || g_state.basswasapi_module == nullptr)
    {
        Log(u8"\u65e0\u6cd5\u4ece\u6e38\u620f\u76ee\u5f55\u6216 runtime \u76ee\u5f55\u52a0\u8f7d\u6240\u9700\u6a21\u5757");
        return false;
    }

    return true;
}

bool ResolveApis()
{
    const ExportBinding bindings[] = {
        BindExport(g_state.bass_module, "BASS_Init", g_api.BASS_Init),
        BindExport(g_state.bass_module, "BASS_Free", g_api.BASS_Free),
        BindExport(g_state.bass_module, "BASS_ErrorGetCode", g_api.BASS_ErrorGetCode),
        BindExport(g_state.bass_module, "BASS_GetDevice", g_api.BASS_GetDevice),
        BindExport(g_state.bass_module, "BASS_GetDeviceInfo", g_api.BASS_GetDeviceInfo),
        BindExport(g_state.bass_module, "BASS_SetDevice", g_api.BASS_SetDevice),
        BindExport(g_state.bass_module, "BASS_ChannelPlay", g_api.BASS_ChannelPlay),
        BindExport(g_state.bass_module, "BASS_ChannelPause", g_api.BASS_ChannelPause),
        BindExport(g_state.bass_module, "BASS_ChannelStop", g_api.BASS_ChannelStop),
        BindExport(g_state.bass_module, "BASS_ChannelIsActive", g_api.BASS_ChannelIsActive),
        BindExport(g_state.bass_module, "BASS_ChannelSetAttribute", g_api.BASS_ChannelSetAttribute),
        BindExport(g_state.bass_module, "BASS_ChannelGetAttribute", g_api.BASS_ChannelGetAttribute),
        BindExport(g_state.bass_module, "BASS_ChannelGetPosition", g_api.BASS_ChannelGetPosition),
        BindExport(g_state.bass_module, "BASS_ChannelSetPosition", g_api.BASS_ChannelSetPosition),
        BindExport(g_state.bass_module, "BASS_ChannelGetLength", g_api.BASS_ChannelGetLength),
        BindExport(g_state.bass_module, "BASS_ChannelGetLevel", g_api.BASS_ChannelGetLevel),
        BindExport(g_state.bass_module, "BASS_ChannelGetData", g_api.BASS_ChannelGetData),
        BindExport(g_state.bass_module, "BASS_StreamCreate", g_api.BASS_StreamCreate),
        BindExport(g_state.bass_module, "BASS_StreamCreateFileUser", g_api.BASS_StreamCreateFileUser),
        BindExport(g_state.bass_module, "BASS_StreamFree", g_api.BASS_StreamFree),
        BindExport(g_state.bass_module, "BASS_SampleGetChannel", g_api.BASS_SampleGetChannel),
        BindExport(g_state.bass_module, "BASS_SampleGetInfo", g_api.BASS_SampleGetInfo),
        BindExport(g_state.bass_module, "BASS_SampleGetData", g_api.BASS_SampleGetData),
        BindExport(g_state.bass_module, "BASS_SampleFree", g_api.BASS_SampleFree),
        BindExport(g_state.bass_fx_module, "BASS_FX_TempoCreate", g_api.BASS_FX_TempoCreate),
        BindExport(g_state.bass_fx_module, "BASS_FX_ReverseCreate", g_api.BASS_FX_ReverseCreate),
        BindExport(g_state.bassmix_module, "BASS_Mixer_StreamCreate", g_api.BASS_Mixer_StreamCreate),
        BindExport(g_state.bassmix_module, "BASS_Mixer_StreamAddChannel", g_api.BASS_Mixer_StreamAddChannel),
        BindExport(g_state.bassmix_module, "BASS_Mixer_ChannelRemove", g_api.BASS_Mixer_ChannelRemove),
        BindExport(g_state.bassmix_module, "BASS_Mixer_ChannelFlags", g_api.BASS_Mixer_ChannelFlags),
        BindExport(g_state.bassmix_module, "BASS_Mixer_ChannelSetPosition", g_api.BASS_Mixer_ChannelSetPosition),
        BindExport(g_state.bassmix_module, "BASS_Mixer_ChannelGetPosition", g_api.BASS_Mixer_ChannelGetPosition),
        BindExport(g_state.bassmix_module, "BASS_Mixer_ChannelGetLevel", g_api.BASS_Mixer_ChannelGetLevel),
        BindExport(g_state.bassmix_module, "BASS_Mixer_ChannelGetData", g_api.BASS_Mixer_ChannelGetData),
        BindExport(g_state.basswasapi_module, "BASS_WASAPI_Init", g_api.BASS_WASAPI_Init),
        BindExport(g_state.basswasapi_module, "BASS_WASAPI_Free", g_api.BASS_WASAPI_Free),
        BindExport(g_state.basswasapi_module, "BASS_WASAPI_Start", g_api.BASS_WASAPI_Start),
        BindExport(g_state.basswasapi_module, "BASS_WASAPI_Stop", g_api.BASS_WASAPI_Stop),
        BindExport(g_state.basswasapi_module, "BASS_WASAPI_GetInfo", g_api.BASS_WASAPI_GetInfo),
        BindExport(g_state.basswasapi_module, "BASS_WASAPI_GetDeviceInfo", g_api.BASS_WASAPI_GetDeviceInfo),
    };
    return ResolveExports(bindings, sizeof(bindings) / sizeof(bindings[0]));
}

WASAPIPROC_NATIVE WasapiProcBass()
{
    // Passing -1 tells BASSWASAPI to pull decoded PCM from the mixer handle in user data.
    return reinterpret_cast<WASAPIPROC_NATIVE>(static_cast<std::intptr_t>(-1));
}

SampleChannelState* FindManagedSample(DWORD handle)
{
    auto it = g_state.sample_channels.find(handle);
    return it == g_state.sample_channels.end() ? nullptr : &it->second;
}

TrackState* FindManagedTrack(DWORD handle)
{
    auto it = g_state.tracks.find(handle);
    return it == g_state.tracks.end() ? nullptr : &it->second;
}

void DetachTrackLocked(DWORD handle, TrackState& state);

BOOL AttachManagedTrackLocked(DWORD handle, TrackState& track)
{
    if (track.attached)
        return TRUE;

    if (g_state.master_mixer == 0 || g_api.BASS_Mixer_StreamAddChannel == nullptr)
        return FALSE;

    if (!g_api.BASS_Mixer_StreamAddChannel(g_state.master_mixer, handle,
                                           BASS_MIXER_CHAN_NORAMPIN | BASS_MIXER_CHAN_BUFFER))
        return FALSE;

    track.attached = true;
    if (track.state != BASS_ACTIVE_PLAYING && g_api.BASS_Mixer_ChannelFlags != nullptr)
        g_api.BASS_Mixer_ChannelFlags(handle, BASS_MIXER_CHAN_PAUSE, BASS_MIXER_CHAN_PAUSE);

    return TRUE;
}

QWORD GetManagedTrackPositionLocked(DWORD handle, const TrackState& track, DWORD mode)
{
    if (track.attached && g_api.BASS_Mixer_ChannelGetPosition != nullptr)
    {
        const QWORD mixer_position = g_api.BASS_Mixer_ChannelGetPosition(handle, mode);
        if (mixer_position != static_cast<QWORD>(-1))
            return mixer_position;
    }

    if (g_api.BASS_ChannelGetPosition == nullptr)
        return static_cast<QWORD>(-1);

    return g_api.BASS_ChannelGetPosition(handle, mode);
}

BOOL SetManagedTrackPositionLocked(DWORD handle, const TrackState& track, QWORD pos, DWORD mode, bool flush_mixer_buffer)
{
    if (track.attached && g_api.BASS_Mixer_ChannelSetPosition != nullptr)
    {
        const DWORD mixer_mode = flush_mixer_buffer ? (mode | BASS_POS_MIXER_RESET) : mode;
        return g_api.BASS_Mixer_ChannelSetPosition(handle, pos, mixer_mode);
    }

    if (g_api.BASS_ChannelSetPosition == nullptr)
        return FALSE;

    TrackState& mutable_track = g_state.tracks[handle];
    const bool was_attached = track.attached;

    if (was_attached)
        DetachTrackLocked(handle, mutable_track);

    const BOOL ok = g_api.BASS_ChannelSetPosition(handle, pos, mode);
    if (!ok)
    {
        if (was_attached)
            AttachManagedTrackLocked(handle, mutable_track);
        return FALSE;
    }

    if (!was_attached)
        return TRUE;

    return AttachManagedTrackLocked(handle, mutable_track);
}

bool HasManagedTrackReachedEndLocked(DWORD handle)
{
    if (g_api.BASS_ChannelGetLength == nullptr || g_api.BASS_ChannelGetPosition == nullptr)
        return false;

    const auto track = g_state.tracks.find(handle);
    if (track == g_state.tracks.end())
        return false;

    const QWORD length = g_api.BASS_ChannelGetLength(handle, BASS_POS_BYTES);
    if (length == 0 || length == static_cast<QWORD>(-1))
        return false;

    const QWORD position = GetManagedTrackPositionLocked(handle, track->second, BASS_POS_BYTES);
    if (position == static_cast<QWORD>(-1))
        return false;

    return position >= length;
}

DWORD CALLBACK SampleStreamProc(DWORD, void* buffer, DWORD length, void* user)
{
    auto* backing = static_cast<SampleBacking*>(user);
    if (backing == nullptr)
        return 0;

    std::lock_guard<std::mutex> lock(backing->mutex);
    if (backing->pcm.empty())
        return 0;

    const std::size_t requested_samples = length / sizeof(float);
    float* out = static_cast<float*>(buffer);
    std::size_t written = 0;

    while (written < requested_samples)
    {
        const std::size_t remaining = backing->pcm.size() - backing->cursor_samples;
        if (remaining == 0)
        {
            if (!backing->loop)
                break;

            backing->cursor_samples = 0;
            continue;
        }

        const std::size_t chunk = std::min(remaining, requested_samples - written);
        std::memcpy(out + written, backing->pcm.data() + backing->cursor_samples, chunk * sizeof(float));
        backing->cursor_samples += chunk;
        written += chunk;
    }

    return static_cast<DWORD>(written * sizeof(float));
}

void DetachTrackLocked(DWORD handle, TrackState& state)
{
    if (state.attached && g_api.BASS_Mixer_ChannelRemove != nullptr)
        g_api.BASS_Mixer_ChannelRemove(handle);

    state.attached = false;
}

void ReleaseSampleBackingLocked(SampleChannelState& sample)
{
    if (sample.attached && g_api.BASS_Mixer_ChannelRemove != nullptr)
        g_api.BASS_Mixer_ChannelRemove(sample.backing_stream);

    sample.attached = false;
    if (sample.backing_stream != 0 && g_api.BASS_StreamFree != nullptr)
        g_api.BASS_StreamFree(sample.backing_stream);

    sample.backing_stream = 0;
    delete sample.backing;
    sample.backing = nullptr;
    sample.state = BASS_ACTIVE_STOPPED;
}

void ApplySampleAttributesLocked(SampleChannelState& sample)
{
    if (sample.backing_stream == 0)
        return;

    g_api.BASS_ChannelSetAttribute(sample.backing_stream, BASS_ATTRIB_VOL, sample.volume);
    g_api.BASS_ChannelSetAttribute(sample.backing_stream, BASS_ATTRIB_PAN, sample.pan);

    if (sample.frequency > 0.0f)
        g_api.BASS_ChannelSetAttribute(sample.backing_stream, BASS_ATTRIB_FREQ, sample.frequency);
}

bool EnsureSampleBackingLocked(SampleChannelState& sample)
{
    if (sample.backing_stream != 0)
        return true;

    BASS_SAMPLE_NATIVE info = {};
    if (!g_api.BASS_SampleGetInfo(sample.sample_handle, &info) || info.length == 0 || info.chans == 0)
    {
        Log(std::string(u8"\u521b\u5efa\u91c7\u6837\u56de\u653e\u6d41\u5931\u8d25: BASS_SampleGetInfo \u8c03\u7528\u5931\u8d25, sample=") +
            std::to_string(sample.sample_handle));
        return false;
    }

    std::vector<std::uint8_t> raw(info.length);
    if (!g_api.BASS_SampleGetData(sample.sample_handle, raw.data()))
    {
        Log(std::string(u8"\u521b\u5efa\u91c7\u6837\u56de\u653e\u6d41\u5931\u8d25: BASS_SampleGetData \u8c03\u7528\u5931\u8d25, sample=") +
            std::to_string(sample.sample_handle));
        return false;
    }

    auto* backing = new SampleBacking();
    backing->channels = info.chans;
    backing->loop = (info.flags & BASS_SAMPLE_LOOP) != 0;

    if ((info.flags & BASS_SAMPLE_FLOAT) != 0)
    {
        const std::size_t sample_count = info.length / sizeof(float);
        backing->pcm.resize(sample_count);
        std::memcpy(backing->pcm.data(), raw.data(), sample_count * sizeof(float));
    }
    else if ((info.flags & BASS_SAMPLE_8BITS) != 0)
    {
        backing->pcm.resize(info.length);
        for (std::size_t i = 0; i < raw.size(); ++i)
            backing->pcm[i] = (static_cast<int>(raw[i]) - 128) / 128.0f;
    }
    else
    {
        const std::size_t sample_count = info.length / sizeof(std::int16_t);
        backing->pcm.resize(sample_count);
        const auto* pcm16 = reinterpret_cast<const std::int16_t*>(raw.data());
        for (std::size_t i = 0; i < sample_count; ++i)
            backing->pcm[i] = pcm16[i] / 32768.0f;
    }

    sample.backing_stream = g_api.BASS_StreamCreate(info.freq, info.chans,
                                                    BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT,
                                                    SampleStreamProc, backing);
    if (sample.backing_stream == 0)
    {
        Log(std::string(u8"\u521b\u5efa\u91c7\u6837\u56de\u653e\u6d41\u5931\u8d25: BASS_StreamCreate \u8c03\u7528\u5931\u8d25, sample=") +
            std::to_string(sample.sample_handle));
        delete backing;
        return false;
    }

    sample.backing = backing;
    sample.frequency = static_cast<float>(info.freq);
    ApplySampleAttributesLocked(sample);
    return true;
}

void ShutdownBridgeLocked()
{
    for (auto& entry : g_state.sample_channels)
        ReleaseSampleBackingLocked(entry.second);
    g_state.sample_channels.clear();

    for (auto& entry : g_state.tracks)
        DetachTrackLocked(entry.first, entry.second);
    g_state.tracks.clear();

    if (g_state.active_wasapi_device >= 0)
    {
        g_api.BASS_WASAPI_Stop(TRUE);
        g_api.BASS_WASAPI_Free();
    }

    if (g_state.master_mixer != 0 && g_api.BASS_StreamFree != nullptr)
    {
        g_api.BASS_StreamFree(g_state.master_mixer);
        g_state.master_mixer = 0;
    }

    g_state.active_wasapi_device = -1;
    g_state.active_bass_device = -1;
}

bool ResolveWasapiDeviceLocked(int& bass_device, int& wasapi_device, std::string& summary)
{
    int resolved_bass_device = bass_device;
    BASS_DEVICEINFO_NATIVE bass_info = {};
    if (resolved_bass_device < 0 || !LoadBassDeviceInfo(resolved_bass_device, bass_info) || !IsUsableBassDevice(bass_info))
        resolved_bass_device = FindPreferredDefaultBassDevice(&bass_info);

    if (resolved_bass_device < 0)
        return false;

    const std::string bass_driver = bass_info.driver == nullptr ? std::string() : bass_info.driver;
    const std::string bass_name = bass_info.name == nullptr ? std::string() : bass_info.name;
    const std::string bass_driver_display = AnsiToUtf8(bass_info.driver);
    const std::string bass_name_display = AnsiToUtf8(bass_info.name);

    auto is_usable_wasapi_output = [](const BASS_WASAPI_DEVICEINFO_NATIVE& info) -> bool
    {
        if ((info.flags & BASS_WASAPI_DEVICE_INPUT) != 0)
            return false;

        if ((info.flags & BASS_WASAPI_DEVICE_LOOPBACK) != 0)
            return false;

        if ((info.flags & BASS_WASAPI_DEVICE_ENABLED) == 0)
            return false;

        if ((info.flags & BASS_WASAPI_DEVICE_DISABLED) != 0)
            return false;

        if ((info.flags & BASS_WASAPI_DEVICE_UNPLUGGED) != 0)
            return false;

        return true;
    };

    // First pass: exact driver/id match only.
    for (int i = 0;; ++i)
    {
        BASS_WASAPI_DEVICEINFO_NATIVE wasapi_info = {};
        if (!g_api.BASS_WASAPI_GetDeviceInfo(i, &wasapi_info))
            break;

        if (!is_usable_wasapi_output(wasapi_info))
            continue;

        const std::string candidate_id = wasapi_info.id == nullptr ? std::string() : wasapi_info.id;
        if (!bass_driver.empty() && candidate_id == bass_driver)
        {
            wasapi_device = i;
            bass_device = resolved_bass_device;
            summary = AnsiToUtf8(wasapi_info.name);
            g_state.active_bass_device = resolved_bass_device;
            return true;
        }
    }

    // Second pass: if the selected BASS device is the default device, let WASAPI resolve
    // its own default output device directly instead of risking a same-name mismatch.
    if ((bass_info.flags & BASS_DEVICE_DEFAULT) != 0)
    {
        wasapi_device = -1;
        bass_device = resolved_bass_device;
        summary = bass_name.empty() ? std::string(u8"\u9ed8\u8ba4\u8bbe\u5907") : bass_name_display;
        g_state.active_bass_device = resolved_bass_device;
        return true;
    }

    // Final fallback: same-name match, but only among enabled render endpoints.
    for (int i = 0;; ++i)
    {
        BASS_WASAPI_DEVICEINFO_NATIVE wasapi_info = {};
        if (!g_api.BASS_WASAPI_GetDeviceInfo(i, &wasapi_info))
            break;

        if (!is_usable_wasapi_output(wasapi_info))
            continue;

        const std::string candidate_name = wasapi_info.name == nullptr ? std::string() : wasapi_info.name;
        if (!bass_name.empty() && candidate_name == bass_name)
        {
            wasapi_device = i;
            bass_device = resolved_bass_device;
            summary = AnsiToUtf8(wasapi_info.name);
            g_state.active_bass_device = resolved_bass_device;
            Log(std::string(u8"\u8bbe\u5907\u6620\u5c04\u56de\u9000\u5230\u540c\u540d WASAPI \u8f93\u51fa\u7aef\u70b9: ") + bass_name_display);
            return true;
        }
    }

    Log(std::string(u8"\u65e0\u6cd5\u4e3a BASS \u8bbe\u5907\u627e\u5230\u53ef\u7528\u7684 WASAPI \u8f93\u51fa\u7aef\u70b9, \u9a71\u52a8='") +
        bass_driver_display + "', " + std::string(u8"\u540d\u79f0='") + bass_name_display + "'");
    return false;
}

bool TryInitializeWasapiLocked(int wasapi_device, DWORD flags, float buffer, float period, const char* label)
{
    if (g_state.master_mixer == 0)
        return false;

    if (g_api.BASS_WASAPI_Init(wasapi_device, 0, 0, flags, buffer, period, WasapiProcBass(),
                               reinterpret_cast<void*>(static_cast<std::uintptr_t>(g_state.master_mixer))))
    {
        Log(std::string(u8"BASS_WASAPI_Init \u6210\u529f, \u914d\u7f6e: ") + label +
            ", mixer=" + std::to_string(g_state.master_mixer) +
            ", flags=0x" + std::to_string(flags));
        return true;
    }

    const int error_code = g_api.BASS_ErrorGetCode != nullptr ? g_api.BASS_ErrorGetCode() : -9999;
    Log(std::string(u8"BASS_WASAPI_Init \u5931\u8d25, \u914d\u7f6e: ") + label +
        ", mixer=" + std::to_string(g_state.master_mixer) +
        ", flags=0x" + std::to_string(flags) +
        ", buffer=" + FormatMilliseconds(buffer) +
        ", period=" + FormatMilliseconds(period) +
        std::string(u8", \u9519\u8bef\u7801=") + std::to_string(error_code));
    return false;
}

bool ConfigureWasapiForMixerLocked(bool exclusive, float buffer_seconds)
{
    if (g_state.master_mixer == 0)
        return false;

    if (g_state.active_wasapi_device >= 0)
    {
        g_api.BASS_WASAPI_Stop(TRUE);
        g_api.BASS_WASAPI_Free();
    }

    const DWORD flags_event = (exclusive ? BASS_WASAPI_EXCLUSIVE : 0) | BASS_WASAPI_EVENT | BASS_WASAPI_AUTOFORMAT;
    const DWORD flags_plain = (exclusive ? BASS_WASAPI_EXCLUSIVE : 0) | BASS_WASAPI_AUTOFORMAT;
    const char* label_event = exclusive ? u8"\u72ec\u5360+\u4e8b\u4ef6\u56de\u8c03" : u8"\u5171\u4eab+\u4e8b\u4ef6\u56de\u8c03";
    const char* label_plain = exclusive ? u8"\u72ec\u5360+\u8f6e\u8be2" : u8"\u5171\u4eab+\u8f6e\u8be2";

    const bool ok =
        TryInitializeWasapiLocked(g_state.active_wasapi_device, flags_event, buffer_seconds, 0.0f, label_event) ||
        TryInitializeWasapiLocked(g_state.active_wasapi_device, flags_plain, buffer_seconds, 0.0f, label_plain);

    if (!ok)
        return false;

    BASS_WASAPI_INFO_NATIVE wasapi_info = {};
    if (!g_api.BASS_WASAPI_GetInfo(&wasapi_info))
    {
        Log(u8"\u914d\u7f6e WASAPI \u540e\u8bfb\u53d6\u8bbe\u5907\u4fe1\u606f\u5931\u8d25");
        g_api.BASS_WASAPI_Free();
        return false;
    }

    Log(std::string(u8"WASAPI \u8bbe\u5907\u4fe1\u606f: \u91c7\u6837\u7387=") + std::to_string(wasapi_info.freq) +
        std::string(u8", \u58f0\u9053=") + std::to_string(wasapi_info.chans) +
        std::string(u8", \u683c\u5f0f=") + std::to_string(wasapi_info.format) +
        std::string(u8", \u7f13\u51b2\u533a\u5b57\u8282\u6570=") + std::to_string(wasapi_info.buflen));

    if (!g_api.BASS_WASAPI_Start())
    {
        Log(u8"\u542f\u52a8 WASAPI \u8f93\u51fa\u5931\u8d25");
        g_api.BASS_WASAPI_Free();
        return false;
    }

    const float latency_seconds =
        (wasapi_info.freq != 0 && wasapi_info.chans != 0)
            ? static_cast<float>(wasapi_info.buflen) / static_cast<float>(wasapi_info.freq * wasapi_info.chans * sizeof(float))
            : 0.0f;
    g_api.BASS_ChannelSetAttribute(g_state.master_mixer, BASS_ATTRIB_MIXER_LATENCY, latency_seconds);
    Log(std::string(u8"WASAPI \u5df2\u914d\u7f6e\u4e3a") +
        (exclusive ? u8"\u72ec\u5360" : u8"\u5171\u4eab") +
        std::string(u8"\u6a21\u5f0f, mixer \u5ef6\u8fdf=") + FormatMilliseconds(latency_seconds));

    return true;
}

bool InitializeBridgeLocked(int bass_device, DWORD freq, DWORD flags, HWND window, void* clsid)
{
    int resolved_bass_device = bass_device;
    int wasapi_device = -1;
    std::string matched_device;
    if (!ResolveWasapiDeviceLocked(resolved_bass_device, wasapi_device, matched_device))
    {
        if (ShouldLogRateLimited(g_state.last_unmapped_device, g_state.last_unmapped_tick, bass_device))
            Log(std::string(u8"\u65e0\u6cd5\u5c06 BASS \u8bbe\u5907\u7d22\u5f15 ") + std::to_string(bass_device) +
                std::string(u8" \u6620\u5c04\u5230 WASAPI \u8f93\u51fa\u7aef\u70b9"));
        return false;
    }

    if (!g_api.BASS_Init(0, freq, flags, window, clsid))
    {
        const int error_code = g_api.BASS_ErrorGetCode != nullptr ? g_api.BASS_ErrorGetCode() : -9999;
        Log(std::string(u8"\u539f\u59cb BASS_Init(\u65e0\u58f0\u8bbe\u5907) \u5931\u8d25, \u9519\u8bef\u7801=") +
            std::to_string(error_code));
        return false;
    }

    BASS_WASAPI_DEVICEINFO_NATIVE endpoint_info = {};
    if (wasapi_device >= 0)
        g_api.BASS_WASAPI_GetDeviceInfo(wasapi_device, &endpoint_info);

    const float preferred_buffer =
        endpoint_info.minperiod > 0.0f ? endpoint_info.minperiod :
        (endpoint_info.defperiod > 0.0f ? endpoint_info.defperiod : 0.03f);
    const float exclusive_buffer = preferred_buffer;
    Log(std::string(u8"\u5df2\u89e3\u6790 WASAPI \u8f93\u51fa\u7aef\u70b9: ") + matched_device +
        std::string(u8", \u6df7\u97f3\u91c7\u6837\u7387=") + std::to_string(endpoint_info.mixfreq) +
        std::string(u8", \u6df7\u97f3\u58f0\u9053=") + std::to_string(endpoint_info.mixchans) +
        std::string(u8", \u6700\u5c0f\u5468\u671f=") + FormatMilliseconds(endpoint_info.minperiod) +
        std::string(u8", \u9ed8\u8ba4\u5468\u671f=") + FormatMilliseconds(endpoint_info.defperiod));

    const DWORD mixer_freq = freq != 0 ? freq : (endpoint_info.mixfreq != 0 ? endpoint_info.mixfreq : 44100);
    const DWORD mixer_chans = endpoint_info.mixchans != 0 ? endpoint_info.mixchans : 2;
    g_state.master_mixer = g_api.BASS_Mixer_StreamCreate(mixer_freq, mixer_chans,
                                                         BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT |
                                                         BASS_MIXER_NONSTOP | BASS_MIXER_POSEX);
    if (g_state.master_mixer == 0)
    {
        g_api.BASS_Free();
        Log(u8"\u521d\u59cb\u5316 WASAPI \u524d\u521b\u5efa\u4e3b mixer \u5931\u8d25");
        return false;
    }

    Log(std::string(u8"\u5df2\u521b\u5efa\u4e3b mixer: handle=") + std::to_string(g_state.master_mixer) +
        std::string(u8", \u91c7\u6837\u7387=") + std::to_string(mixer_freq) +
        std::string(u8", \u58f0\u9053=") + std::to_string(mixer_chans));

    g_state.active_wasapi_device = wasapi_device;
    const bool wasapi_ok = ConfigureWasapiForMixerLocked(true, exclusive_buffer);

    if (!wasapi_ok)
    {
        if (g_state.master_mixer != 0)
        {
            g_api.BASS_StreamFree(g_state.master_mixer);
            g_state.master_mixer = 0;
        }
        g_api.BASS_Free();
        ::InterlockedExchange(&g_state.block_next_fallback, 1);
        if (ShouldLogRateLimited(g_state.last_wasapi_failure_device, g_state.last_wasapi_failure_tick, wasapi_device))
            Log(std::string(u8"WASAPI \u72ec\u5360\u521d\u59cb\u5316\u5931\u8d25, \u7aef\u70b9: ") + matched_device);
        return false;
    }

    g_state.bass_initialized = true;
    g_state.active_bass_device = resolved_bass_device;
    Log(std::string(u8"\u97f3\u9891\u6865\u5df2\u521d\u59cb\u5316: BASS \u8bbe\u5907 ") +
        std::to_string(resolved_bass_device) +
        std::string(u8" \u6620\u5c04\u5230 ") + matched_device +
        std::string(u8" (\u72ec\u5360)"));
    return true;
}

DWORD AllocateLogicalHandle()
{
    return g_state.next_logical_handle.fetch_add(1);
}

BOOL StartManagedTrackLocked(DWORD handle, TrackState& track, BOOL restart)
{
    if (g_state.master_mixer == 0)
        return FALSE;

    if (restart)
    {
        if (!SetManagedTrackPositionLocked(handle, track, 0, BASS_POS_BYTES, true))
            return FALSE;
    }

    if (!track.attached)
    {
        if (!AttachManagedTrackLocked(handle, track))
            return FALSE;

        if (g_api.BASS_Mixer_ChannelFlags != nullptr)
            g_api.BASS_Mixer_ChannelFlags(handle, 0, BASS_MIXER_CHAN_PAUSE);
    }
    else
    {
        g_api.BASS_Mixer_ChannelFlags(handle, 0, BASS_MIXER_CHAN_PAUSE);
    }

    track.state = BASS_ACTIVE_PLAYING;
    return TRUE;
}

BOOL PauseManagedTrackLocked(DWORD handle, TrackState& track)
{
    if (track.attached)
        g_api.BASS_Mixer_ChannelFlags(handle, BASS_MIXER_CHAN_PAUSE, BASS_MIXER_CHAN_PAUSE);

    track.state = BASS_ACTIVE_PAUSED;
    return TRUE;
}

BOOL StopManagedTrackLocked(DWORD handle, TrackState& track)
{
    DetachTrackLocked(handle, track);
    SetManagedTrackPositionLocked(handle, track, 0, BASS_POS_BYTES, false);
    track.state = BASS_ACTIVE_STOPPED;
    return TRUE;
}

BOOL StartManagedSampleLocked(SampleChannelState& sample, BOOL restart)
{
    if (!EnsureSampleBackingLocked(sample) || g_state.master_mixer == 0)
        return FALSE;

    if (restart)
    {
        if (sample.backing != nullptr)
        {
            std::lock_guard<std::mutex> sample_lock(sample.backing->mutex);
            sample.backing->cursor_samples = 0;
        }
    }

    if (!sample.attached)
    {
        if (!g_api.BASS_Mixer_StreamAddChannel(g_state.master_mixer, sample.backing_stream,
                                               BASS_MIXER_CHAN_NORAMPIN | BASS_MIXER_CHAN_BUFFER))
            return FALSE;

        sample.attached = true;
    }
    else
    {
        g_api.BASS_Mixer_ChannelFlags(sample.backing_stream, 0, BASS_MIXER_CHAN_PAUSE);
    }

    sample.state = BASS_ACTIVE_PLAYING;
    return TRUE;
}

BOOL PauseManagedSampleLocked(SampleChannelState& sample)
{
    if (sample.backing_stream != 0 && sample.attached)
        g_api.BASS_Mixer_ChannelFlags(sample.backing_stream, BASS_MIXER_CHAN_PAUSE, BASS_MIXER_CHAN_PAUSE);

    sample.state = BASS_ACTIVE_PAUSED;
    return TRUE;
}

BOOL StopManagedSampleLocked(SampleChannelState& sample)
{
    ReleaseSampleBackingLocked(sample);
    return TRUE;
}

extern "C" BOOL WINAPI Hook_BASS_Init(int device, DWORD freq, DWORD flags, HWND window, void* clsid)
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);

    const LONG fallback_budget = ::InterlockedCompareExchange(&g_state.block_next_fallback, 0, 0);
    if (fallback_budget > 0)
    {
        ::InterlockedExchangeAdd(&g_state.block_next_fallback, -1);
        Log(u8"\u5df2\u62e6\u622a\u7d27\u968f\u5176\u540e\u7684 BASS_Init \u56de\u9000\u5c1d\u8bd5");
        return FALSE;
    }

    ShutdownBridgeLocked();
    if (g_state.bass_initialized)
    {
        g_api.BASS_Free();
        g_state.bass_initialized = false;
    }

    return InitializeBridgeLocked(device, freq, flags, window, clsid) ? TRUE : FALSE;
}

extern "C" int WINAPI Hook_BASS_GetDevice()
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);
    if (g_state.active_bass_device >= 0)
        return g_state.active_bass_device;

    const int preferred_default = FindPreferredDefaultBassDevice();
    if (preferred_default >= 0)
        return preferred_default;

    return g_api.BASS_GetDevice();
}

extern "C" BOOL WINAPI Hook_BASS_GetDeviceInfo(int device, BASS_DEVICEINFO_NATIVE* info)
{
    if (!g_api.BASS_GetDeviceInfo(device, info))
        return FALSE;

    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);
    const int preferred_default = FindPreferredDefaultBassDevice();
    if (device == preferred_default && info != nullptr && info->driver != nullptr)
        info->flags |= BASS_DEVICE_DEFAULT;

    if (device == g_state.active_bass_device && info != nullptr)
        info->flags |= BASS_DEVICE_INIT;

    return TRUE;
}

extern "C" BOOL WINAPI Hook_BASS_SetDevice(int device)
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);
    if (device < 0)
    {
        g_state.active_bass_device = FindPreferredDefaultBassDevice();
        return TRUE;
    }

    BASS_DEVICEINFO_NATIVE info = {};
    if (!g_api.BASS_GetDeviceInfo(device, &info))
        return FALSE;

    g_state.active_bass_device = device;
    return TRUE;
}

extern "C" BOOL WINAPI Hook_BASS_Free()
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);
    ShutdownBridgeLocked();
    g_state.bass_initialized = false;
    ::InterlockedExchange(&g_state.block_next_fallback, 0);
    Log(u8"\u5df2\u62e6\u622a BASS_Free, \u5e76\u5b8c\u6210\u6865\u63a5\u6e05\u7406");
    return g_api.BASS_Free();
}

extern "C" DWORD WINAPI Hook_BASS_StreamCreateFileUser(DWORD system, DWORD flags, const BASS_FILEPROCS_NATIVE* procs, void* user)
{
    DWORD patched_flags = flags | BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT;
    if (flags == 0)
        patched_flags |= BASS_STREAM_PRESCAN;

    const DWORD source_handle = g_api.BASS_StreamCreateFileUser(system, patched_flags, procs, user);
    if (source_handle == 0)
        return 0;

    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);
    g_state.tracks.emplace(source_handle, TrackState {});
    return source_handle;
}

extern "C" BOOL WINAPI Hook_BASS_StreamFree(DWORD handle)
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);
    TrackState* track = FindManagedTrack(handle);
    if (track != nullptr)
    {
        DetachTrackLocked(handle, *track);
        g_state.tracks.erase(handle);
    }

    return g_api.BASS_StreamFree(handle);
}

extern "C" DWORD WINAPI Hook_BASS_SampleGetChannel(DWORD handle, BOOL only_new)
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);

    SampleChannelState sample = {};
    sample.logical_handle = AllocateLogicalHandle();
    sample.sample_handle = handle;
    (void)only_new;

    auto inserted = g_state.sample_channels.emplace(sample.logical_handle, sample);
    SampleChannelState& stored = inserted.first->second;
    if (!EnsureSampleBackingLocked(stored))
    {
        Log(std::string(u8"\u521b\u5efa\u91c7\u6837\u901a\u9053\u5931\u8d25: \u65e0\u6cd5\u5efa\u7acb\u56de\u653e\u6d41, sample=") +
            std::to_string(handle));
        g_state.sample_channels.erase(sample.logical_handle);
        return 0;
    }

    return stored.logical_handle;
}

extern "C" BOOL WINAPI Hook_BASS_SampleFree(DWORD handle)
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);
    for (auto it = g_state.sample_channels.begin(); it != g_state.sample_channels.end();)
    {
        if (it->second.sample_handle != handle)
        {
            ++it;
            continue;
        }

        ReleaseSampleBackingLocked(it->second);
        it = g_state.sample_channels.erase(it);
    }

    return g_api.BASS_SampleFree(handle);
}

extern "C" DWORD WINAPI Hook_BASS_FX_TempoCreate(DWORD channel, DWORD flags)
{
    const DWORD handle = g_api.BASS_FX_TempoCreate(channel, flags | BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT);
    if (handle == 0)
        return 0;

    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);
    g_state.tracks.emplace(handle, TrackState {});
    return handle;
}

extern "C" DWORD WINAPI Hook_BASS_FX_ReverseCreate(DWORD channel, float dec_block, DWORD flags)
{
    const DWORD handle = g_api.BASS_FX_ReverseCreate(channel, dec_block, flags | BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT);
    if (handle == 0)
        return 0;

    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);
    g_state.tracks.emplace(handle, TrackState {});
    return handle;
}

extern "C" BOOL WINAPI Hook_BASS_ChannelPlay(DWORD handle, BOOL restart)
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);

    SampleChannelState* sample = FindManagedSample(handle);
    if (sample != nullptr)
        return StartManagedSampleLocked(*sample, restart);

    TrackState* track = FindManagedTrack(handle);
    if (track != nullptr)
        return StartManagedTrackLocked(handle, *track, restart);

    return g_api.BASS_ChannelPlay(handle, restart);
}

extern "C" BOOL WINAPI Hook_BASS_ChannelPause(DWORD handle)
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);

    SampleChannelState* sample = FindManagedSample(handle);
    if (sample != nullptr)
        return PauseManagedSampleLocked(*sample);

    TrackState* track = FindManagedTrack(handle);
    if (track != nullptr)
        return PauseManagedTrackLocked(handle, *track);

    return g_api.BASS_ChannelPause(handle);
}

extern "C" BOOL WINAPI Hook_BASS_ChannelStop(DWORD handle)
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);

    SampleChannelState* sample = FindManagedSample(handle);
    if (sample != nullptr)
        return StopManagedSampleLocked(*sample);

    TrackState* track = FindManagedTrack(handle);
    if (track != nullptr)
        return StopManagedTrackLocked(handle, *track);

    return g_api.BASS_ChannelStop(handle);
}

extern "C" DWORD WINAPI Hook_BASS_ChannelIsActive(DWORD handle)
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);

    SampleChannelState* sample = FindManagedSample(handle);
    if (sample != nullptr)
    {
        bool should_release = false;
        if (sample->state == BASS_ACTIVE_PLAYING && sample->backing != nullptr)
        {
            std::lock_guard<std::mutex> sample_lock(sample->backing->mutex);
            should_release = !sample->backing->loop && sample->backing->cursor_samples >= sample->backing->pcm.size();
        }

        if (should_release)
            ReleaseSampleBackingLocked(*sample);

        return sample->state;
    }

    TrackState* track = FindManagedTrack(handle);
    if (track != nullptr)
    {
        if (track->state == BASS_ACTIVE_PLAYING &&
            HasManagedTrackReachedEndLocked(handle))
            StopManagedTrackLocked(handle, *track);

        return track->state;
    }

    return g_api.BASS_ChannelIsActive(handle);
}

extern "C" BOOL WINAPI Hook_BASS_ChannelSetAttribute(DWORD handle, DWORD attrib, float value)
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);

    SampleChannelState* sample = FindManagedSample(handle);
    if (sample != nullptr)
    {
        if (attrib == BASS_ATTRIB_VOL)
            sample->volume = value;
        else if (attrib == BASS_ATTRIB_PAN)
            sample->pan = value;
        else if (attrib == BASS_ATTRIB_FREQ)
            sample->frequency = value;

        ApplySampleAttributesLocked(*sample);
        return TRUE;
    }

    return g_api.BASS_ChannelSetAttribute(handle, attrib, value);
}

extern "C" DWORD WINAPI Hook_BASS_ChannelGetLevel(DWORD handle)
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);

    TrackState* track = FindManagedTrack(handle);
    if (track != nullptr &&
        track->attached &&
        g_api.BASS_Mixer_ChannelGetLevel != nullptr)
    {
        const DWORD level = g_api.BASS_Mixer_ChannelGetLevel(handle);
        if (level != static_cast<DWORD>(-1))
            return level;
    }

    if (g_api.BASS_ChannelGetLevel == nullptr)
        return static_cast<DWORD>(-1);

    return g_api.BASS_ChannelGetLevel(handle);
}

extern "C" DWORD WINAPI Hook_BASS_ChannelGetData(DWORD handle, void* buffer, DWORD length)
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);

    TrackState* track = FindManagedTrack(handle);
    if (track != nullptr &&
        track->attached &&
        g_api.BASS_Mixer_ChannelGetData != nullptr)
    {
        const DWORD copied = g_api.BASS_Mixer_ChannelGetData(handle, buffer, length);
        if (copied != 0xFFFFFFFF)
            return copied;
    }

    if (g_api.BASS_ChannelGetData == nullptr)
        return 0xFFFFFFFF;

    return g_api.BASS_ChannelGetData(handle, buffer, length);
}

extern "C" BOOL WINAPI Hook_BASS_ChannelGetAttribute(DWORD handle, DWORD attrib, float* value)
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);

    SampleChannelState* sample = FindManagedSample(handle);
    if (sample == nullptr)
        return g_api.BASS_ChannelGetAttribute(handle, attrib, value);

    if (value == nullptr)
        return FALSE;

    if (attrib == BASS_ATTRIB_VOL)
    {
        *value = sample->volume;
        return TRUE;
    }

    if (attrib == BASS_ATTRIB_PAN)
    {
        *value = sample->pan;
        return TRUE;
    }

    if (attrib == BASS_ATTRIB_FREQ)
    {
        *value = sample->frequency;
        return TRUE;
    }

    if (sample->backing_stream == 0)
        return FALSE;

    return g_api.BASS_ChannelGetAttribute(sample->backing_stream, attrib, value);
}

extern "C" BOOL WINAPI Hook_BASS_ChannelSetPosition(DWORD handle, QWORD pos, DWORD mode)
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);

    SampleChannelState* sample = FindManagedSample(handle);
    if (sample != nullptr)
    {
        if (!EnsureSampleBackingLocked(*sample))
            return FALSE;

        if (sample->backing != nullptr)
        {
            std::lock_guard<std::mutex> sample_lock(sample->backing->mutex);
            sample->backing->cursor_samples = static_cast<std::size_t>(pos / sizeof(float));
            if (sample->backing->cursor_samples > sample->backing->pcm.size())
                sample->backing->cursor_samples = sample->backing->pcm.size();
            return TRUE;
        }

        return FALSE;
    }

    TrackState* track = FindManagedTrack(handle);
    if (track != nullptr)
        return SetManagedTrackPositionLocked(handle, *track, pos, mode, true);

    return g_api.BASS_ChannelSetPosition(handle, pos, mode);
}

extern "C" QWORD WINAPI Hook_BASS_ChannelGetPosition(DWORD handle, DWORD mode)
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);

    (void)mode;

    SampleChannelState* sample = FindManagedSample(handle);
    if (sample != nullptr && sample->backing != nullptr)
    {
        std::lock_guard<std::mutex> sample_lock(sample->backing->mutex);
        return static_cast<QWORD>(sample->backing->cursor_samples * sizeof(float));
    }

    if (sample != nullptr && sample->attached && sample->backing_stream != 0 &&
        g_api.BASS_Mixer_ChannelGetPosition != nullptr)
    {
        return g_api.BASS_Mixer_ChannelGetPosition(sample->backing_stream, mode);
    }

    if (sample != nullptr && sample->backing_stream != 0)
        return g_api.BASS_ChannelGetPosition(sample->backing_stream, mode);

    TrackState* track = FindManagedTrack(handle);
    if (track != nullptr)
        return GetManagedTrackPositionLocked(handle, *track, mode);

    return g_api.BASS_ChannelGetPosition(handle, mode);
}

bool PatchExports()
{
    const ExportPatch patches[] = {
        { g_state.bass_module, "BASS_Init", reinterpret_cast<void*>(Hook_BASS_Init) },
        { g_state.bass_module, "BASS_GetDevice", reinterpret_cast<void*>(Hook_BASS_GetDevice) },
        { g_state.bass_module, "BASS_GetDeviceInfo", reinterpret_cast<void*>(Hook_BASS_GetDeviceInfo) },
        { g_state.bass_module, "BASS_SetDevice", reinterpret_cast<void*>(Hook_BASS_SetDevice) },
        { g_state.bass_module, "BASS_Free", reinterpret_cast<void*>(Hook_BASS_Free) },
        { g_state.bass_module, "BASS_StreamCreateFileUser", reinterpret_cast<void*>(Hook_BASS_StreamCreateFileUser) },
        { g_state.bass_module, "BASS_StreamFree", reinterpret_cast<void*>(Hook_BASS_StreamFree) },
        { g_state.bass_module, "BASS_SampleGetChannel", reinterpret_cast<void*>(Hook_BASS_SampleGetChannel) },
        { g_state.bass_module, "BASS_SampleFree", reinterpret_cast<void*>(Hook_BASS_SampleFree) },
        { g_state.bass_module, "BASS_ChannelPlay", reinterpret_cast<void*>(Hook_BASS_ChannelPlay) },
        { g_state.bass_module, "BASS_ChannelPause", reinterpret_cast<void*>(Hook_BASS_ChannelPause) },
        { g_state.bass_module, "BASS_ChannelStop", reinterpret_cast<void*>(Hook_BASS_ChannelStop) },
        { g_state.bass_module, "BASS_ChannelIsActive", reinterpret_cast<void*>(Hook_BASS_ChannelIsActive) },
        { g_state.bass_module, "BASS_ChannelSetAttribute", reinterpret_cast<void*>(Hook_BASS_ChannelSetAttribute) },
        { g_state.bass_module, "BASS_ChannelGetLevel", reinterpret_cast<void*>(Hook_BASS_ChannelGetLevel) },
        { g_state.bass_module, "BASS_ChannelGetData", reinterpret_cast<void*>(Hook_BASS_ChannelGetData) },
        { g_state.bass_module, "BASS_ChannelGetAttribute", reinterpret_cast<void*>(Hook_BASS_ChannelGetAttribute) },
        { g_state.bass_module, "BASS_ChannelGetPosition", reinterpret_cast<void*>(Hook_BASS_ChannelGetPosition) },
        { g_state.bass_module, "BASS_ChannelSetPosition", reinterpret_cast<void*>(Hook_BASS_ChannelSetPosition) },
        { g_state.bass_fx_module, "BASS_FX_TempoCreate", reinterpret_cast<void*>(Hook_BASS_FX_TempoCreate) },
        { g_state.bass_fx_module, "BASS_FX_ReverseCreate", reinterpret_cast<void*>(Hook_BASS_FX_ReverseCreate) },
    };

    if (!ApplyExportPatches(patches, sizeof(patches) / sizeof(patches[0])))
    {
        Log(u8"\u4fee\u8865 BASS \u5bfc\u51fa\u8868\u5931\u8d25");
        return false;
    }

    return true;
}
} // namespace

extern "C" __declspec(dllexport) DWORD WINAPI InitializeHookBridge(LPVOID)
{
    std::lock_guard<std::recursive_mutex> lock(g_state.mutex);
    if (g_state.initialized)
        return ERROR_ALREADY_INITIALIZED;

    LoadRuntimePaths();
    Log(u8"\u5f00\u59cb\u521d\u59cb\u5316\u97f3\u9891 HOOK \u6865\u63a5");

    if (!LoadModules())
    {
        Log(u8"\u521d\u59cb\u5316\u5931\u8d25: ERROR_MOD_NOT_FOUND");
        return ERROR_MOD_NOT_FOUND;
    }

    if (!ResolveApis())
    {
        Log(u8"\u521d\u59cb\u5316\u5931\u8d25: ERROR_PROC_NOT_FOUND");
        return ERROR_PROC_NOT_FOUND;
    }

    if (!PatchExports())
    {
        Log(u8"\u521d\u59cb\u5316\u5931\u8d25: \u65e0\u6cd5\u4fee\u8865 BASS \u5bfc\u51fa\u8868");
        return ERROR_INVALID_FUNCTION;
    }

    g_state.initialized = true;
    Log(u8"\u97f3\u9891 HOOK \u6865\u63a5\u521d\u59cb\u5316\u5b8c\u6210");
    return ERROR_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_state.self = instance;
        ::DisableThreadLibraryCalls(instance);
    }

    return TRUE;
}
