# WASAPI Exclusive Audio Hook

这个工作区包含一个独立的 `x86` 启动器和一个注入式 Hook DLL，用于强制
osu! 的 BASS 音频输出路径通过 `basswasapi.dll` 以独占模式运行。

## Layout

- `common/`：用于 BASS/BASS_FX/BASSMIX/BASSWASAPI 互操作的共享头文件，
  以及少量 Win32 辅助工具。
- `launcher/`：`launcher.exe` 所在目录。它会以挂起方式启动 `osu!.exe`，
  注入 Hook DLL，调用导出的初始化函数，然后恢复游戏进程。
- `hook/`：`osu_audio_hook.dll` 所在目录。该 DLL 会预加载 BASS，修补导出
  地址表，虚拟化设备选择，并将播放链路转接到 `bassmix + basswasapi`。
- `runtime/`：构建后的 `launcher.exe`、Hook DLL 和相关附加模块的运行时输出目录。
- `logs/`：启动器和 Hook 在运行时写出的文本日志。
- `third_party/`：用于放置可选的外部源码或说明文件。

## Build

1. 在安装了 `Desktop development with C++` 的 Visual Studio 中打开 `wasapi.sln`。
2. 选择 `Debug|Win32` 或 `Release|Win32`。
3. 构建两个项目。

解决方案会将 `launcher.exe` 和 `osu_audio_hook.dll` 输出到 `runtime/`。

## Runtime Dependencies

将以下 `x86` 附加模块与构建产物一起放到 `runtime/` 中：

- `bassmix.dll`
- `basswasapi.dll`

Hook 会从游戏目录加载 `bass.dll` 和 `bass_fx.dll`，而不是从 `runtime/` 加载。

## Launch

```powershell
runtime\launcher.exe --game "C:\path\to\osu!.exe"
```

## Notes

- Hook 需要目标游戏使用的 `bass.dll` 版本为 `2.4.15.2`。打破这个假设就会失效。
- 设备选择仍然由游戏自身的 BASS 设备列表驱动。
- 当前初始化过程中如果独占模式失败，会被视为硬失败；紧随其后的“回退到默认
  设备再重试”会被阻止。
