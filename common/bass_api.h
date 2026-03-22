#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <cstdint>

namespace wasapi_bass
{
using QWORD = unsigned long long;

constexpr DWORD BASS_DEVICE_ENABLED = 0x1;
constexpr DWORD BASS_DEVICE_DEFAULT = 0x2;
constexpr DWORD BASS_DEVICE_INIT = 0x4;

constexpr DWORD BASS_POS_BYTES = 0;
constexpr DWORD BASS_POS_MIXER_RESET = 0x10000;

constexpr DWORD BASS_ACTIVE_STOPPED = 0;
constexpr DWORD BASS_ACTIVE_PLAYING = 1;
constexpr DWORD BASS_ACTIVE_PAUSED = 3;

constexpr DWORD BASS_ATTRIB_FREQ = 1;
constexpr DWORD BASS_ATTRIB_VOL = 2;
constexpr DWORD BASS_ATTRIB_PAN = 3;
constexpr DWORD BASS_ATTRIB_MIXER_LATENCY = 0x15000;

constexpr DWORD BASS_SAMPLE_8BITS = 0x1;
constexpr DWORD BASS_SAMPLE_LOOP = 0x4;
constexpr DWORD BASS_SAMPLE_FLOAT = 0x100;
constexpr DWORD BASS_STREAM_PRESCAN = 0x20000;
constexpr DWORD BASS_STREAM_DECODE = 0x200000;

constexpr DWORD BASS_MIXER_CHAN_BUFFER = 0x2000;
constexpr DWORD BASS_MIXER_CHAN_PAUSE = 0x20000;
constexpr DWORD BASS_MIXER_CHAN_NORAMPIN = 0x800000;
constexpr DWORD BASS_MIXER_NONSTOP = 0x20000;
constexpr DWORD BASS_MIXER_POSEX = 0x2000;

constexpr DWORD BASS_WASAPI_EXCLUSIVE = 0x1;
constexpr DWORD BASS_WASAPI_AUTOFORMAT = 0x2;
constexpr DWORD BASS_WASAPI_EVENT = 0x10;

constexpr DWORD BASS_WASAPI_DEVICE_ENABLED = 0x1;
constexpr DWORD BASS_WASAPI_DEVICE_LOOPBACK = 0x8;
constexpr DWORD BASS_WASAPI_DEVICE_INPUT = 0x10;
constexpr DWORD BASS_WASAPI_DEVICE_UNPLUGGED = 0x20;
constexpr DWORD BASS_WASAPI_DEVICE_DISABLED = 0x40;

struct BASS_DEVICEINFO_NATIVE
{
    const char* name;
    const char* driver;
    DWORD flags;
};

struct BASS_SAMPLE_NATIVE
{
    DWORD freq;
    float volume;
    float pan;
    DWORD flags;
    DWORD length;
    DWORD max;
    DWORD origres;
    DWORD chans;
    DWORD mingap;
    DWORD mode3d;
    float mindist;
    float maxdist;
    DWORD iangle;
    DWORD oangle;
    float outvol;
    DWORD vam;
    DWORD priority;
};

using FILECLOSEPROC_NATIVE = void(CALLBACK*)(void* user);
using FILELENPROC_NATIVE = QWORD(CALLBACK*)(void* user);
using FILEREADPROC_NATIVE = DWORD(CALLBACK*)(void* buffer, DWORD length, void* user);
using FILESEEKPROC_NATIVE = BOOL(CALLBACK*)(QWORD offset, void* user);

struct BASS_FILEPROCS_NATIVE
{
    FILECLOSEPROC_NATIVE close;
    FILELENPROC_NATIVE length;
    FILEREADPROC_NATIVE read;
    FILESEEKPROC_NATIVE seek;
};

struct BASS_WASAPI_DEVICEINFO_NATIVE
{
    const char* name;
    const char* id;
    DWORD type;
    DWORD flags;
    float minperiod;
    float defperiod;
    DWORD mixfreq;
    DWORD mixchans;
};

struct BASS_WASAPI_INFO_NATIVE
{
    DWORD initflags;
    DWORD freq;
    DWORD chans;
    DWORD format;
    DWORD buflen;
    float volmax;
    float volmin;
    float volstep;
};

using WASAPIPROC_NATIVE = DWORD(CALLBACK*)(void* buffer, DWORD length, void* user);

using BASS_Init_Fn = BOOL(WINAPI*)(int device, DWORD freq, DWORD flags, HWND win, void* clsid);
using BASS_Free_Fn = BOOL(WINAPI*)();
using BASS_ErrorGetCode_Fn = int(WINAPI*)();
using BASS_GetDevice_Fn = int(WINAPI*)();
using BASS_GetDeviceInfo_Fn = BOOL(WINAPI*)(int device, BASS_DEVICEINFO_NATIVE* info);
using BASS_SetDevice_Fn = BOOL(WINAPI*)(int device);
using BASS_ChannelPlay_Fn = BOOL(WINAPI*)(DWORD handle, BOOL restart);
using BASS_ChannelPause_Fn = BOOL(WINAPI*)(DWORD handle);
using BASS_ChannelStop_Fn = BOOL(WINAPI*)(DWORD handle);
using BASS_ChannelIsActive_Fn = DWORD(WINAPI*)(DWORD handle);
using BASS_ChannelSetAttribute_Fn = BOOL(WINAPI*)(DWORD handle, DWORD attrib, float value);
using BASS_ChannelGetAttribute_Fn = BOOL(WINAPI*)(DWORD handle, DWORD attrib, float* value);
using BASS_ChannelGetPosition_Fn = QWORD(WINAPI*)(DWORD handle, DWORD mode);
using BASS_ChannelSetPosition_Fn = BOOL(WINAPI*)(DWORD handle, QWORD pos, DWORD mode);
using BASS_ChannelGetLength_Fn = QWORD(WINAPI*)(DWORD handle, DWORD mode);
using BASS_ChannelGetLevel_Fn = DWORD(WINAPI*)(DWORD handle);
using BASS_ChannelGetData_Fn = DWORD(WINAPI*)(DWORD handle, void* buffer, DWORD length);
using STREAMPROC_NATIVE = DWORD(CALLBACK*)(DWORD handle, void* buffer, DWORD length, void* user);
using BASS_StreamCreate_Fn = DWORD(WINAPI*)(DWORD freq, DWORD chans, DWORD flags, STREAMPROC_NATIVE proc, void* user);
using BASS_StreamCreateFileUser_Fn = DWORD(WINAPI*)(DWORD system, DWORD flags, const BASS_FILEPROCS_NATIVE* procs,
                                                     void* user);
using BASS_StreamFree_Fn = BOOL(WINAPI*)(DWORD handle);
using BASS_SampleGetChannel_Fn = DWORD(WINAPI*)(DWORD handle, BOOL onlynew);
using BASS_SampleGetInfo_Fn = BOOL(WINAPI*)(DWORD handle, BASS_SAMPLE_NATIVE* info);
using BASS_SampleGetData_Fn = BOOL(WINAPI*)(DWORD handle, void* buffer);
using BASS_SampleFree_Fn = BOOL(WINAPI*)(DWORD handle);

using BASS_FX_TempoCreate_Fn = DWORD(WINAPI*)(DWORD channel, DWORD flags);
using BASS_FX_ReverseCreate_Fn = DWORD(WINAPI*)(DWORD channel, float dec_block, DWORD flags);

using BASS_Mixer_StreamCreate_Fn = DWORD(WINAPI*)(DWORD freq, DWORD chans, DWORD flags);
using BASS_Mixer_StreamAddChannel_Fn = BOOL(WINAPI*)(DWORD handle, DWORD channel, DWORD flags);
using BASS_Mixer_ChannelRemove_Fn = BOOL(WINAPI*)(DWORD handle);
using BASS_Mixer_ChannelFlags_Fn = DWORD(WINAPI*)(DWORD handle, DWORD flags, DWORD mask);
using BASS_Mixer_ChannelSetPosition_Fn = BOOL(WINAPI*)(DWORD handle, QWORD pos, DWORD mode);
using BASS_Mixer_ChannelGetPosition_Fn = QWORD(WINAPI*)(DWORD handle, DWORD mode);
using BASS_Mixer_ChannelGetLevel_Fn = DWORD(WINAPI*)(DWORD handle);
using BASS_Mixer_ChannelGetData_Fn = DWORD(WINAPI*)(DWORD handle, void* buffer, DWORD length);

using BASS_WASAPI_Init_Fn = BOOL(WINAPI*)(int device, DWORD freq, DWORD chans, DWORD flags, float buffer, float period,
                                          WASAPIPROC_NATIVE proc, void* user);
using BASS_WASAPI_Free_Fn = BOOL(WINAPI*)();
using BASS_WASAPI_Start_Fn = BOOL(WINAPI*)();
using BASS_WASAPI_Stop_Fn = BOOL(WINAPI*)(BOOL reset);
using BASS_WASAPI_GetInfo_Fn = BOOL(WINAPI*)(BASS_WASAPI_INFO_NATIVE* info);
using BASS_WASAPI_GetDeviceInfo_Fn = BOOL(WINAPI*)(int device, BASS_WASAPI_DEVICEINFO_NATIVE* info);
} // namespace wasapi_bass
