# WASAPI 独占模式音频钩子

这个工作区包含一个独立的 `x86` 启动器和一个注入式 Hook DLL，用于强制
osu! 的 BASS 音频输出路径通过 `basswasapi.dll` 以独占模式运行。

## 代码布局

- `common/`：用于 BASS/BASS_FX/BASSMIX/BASSWASAPI 互操作的共享头文件，
  以及少量 Win32 辅助工具。
- `launcher/`：`launcher.exe` 所在目录。它会以挂起方式启动 `osu!.exe`，
  注入 Hook DLL，调用导出的初始化函数，然后恢复游戏进程。
- `hook/`：`osu_audio_hook.dll` 所在目录。该 DLL 会预加载 BASS，修补导出
  地址表，虚拟化设备选择，并将播放链路转接到 `bassmix + basswasapi`。
- `runtime/`：构建后的 `launcher.exe`、Hook DLL 和相关附加模块的运行时输出目录。
- `logs/`：启动器和 Hook 在运行时写出的文本日志。
- `third_party/`：用于放置可选的外部源码或说明文件。

## 构建

1. 在安装了 `Desktop development with C++` 的 Visual Studio 中打开 `wasapi.sln`。
2. 选择 `Debug|Win32` 或 `Release|Win32`。
3. 构建两个项目。

解决方案会将 `launcher.exe` 和 `osu_audio_hook.dll` 输出到 `runtime/`。

## 运行时依赖

将以下 `x86` 附加模块与构建产物一起放到 `runtime/` 中：

- `bassmix.dll`
- `basswasapi.dll`

Hook 会从游戏目录加载 `bass.dll` 和 `bass_fx.dll`，而不是从 `runtime/` 加载。

## 启动方式

```powershell
runtime\launcher.exe --game "C:\path\to\osu!.exe"
```

## 备注

- Hook 需要目标游戏使用的 `bass.dll` 版本为 `2.4.15.2`。打破这个假设，钩子就会失效。
- 启动后需要在游戏内拉-25ms的全局Offset。为什么是这个值我也不知道，我从McOsu的WASAPI独占抄的，据[他们](https://github.com/McKay42/McOsu/blob/master/src/App/Osu/Osu.cpp#L241)说是因为WASAPI独占会使用新版本的bass相关dll，行为和旧版不一样
- 设备选择仍然由游戏自身的 BASS 设备列表驱动。
- 当前初始化过程中如果独占模式失败，会被视为硬失败；紧随其后的“回退到默认
  设备再重试”会被阻止。

## 碎碎念

这个项目一开始是由于我买了个游戏本而触发的，在公司摸鱼玩osu，别的问题都解决了，键盘可以用MeowPad的触盘，轴刚好前段时间刚出了个新结构静音磁轴；唯一的问题是低延迟音效，背着跑的电脑肯定不能带个硕大的拓品声卡，何况这种场景我也不需要48V输入来接电容麦；

唯一可行的方案就是USB小尾巴（DAC），但是带ASIO的USB小尾巴又不好找，好不容易找到个Fiio的KA11，不知道是因为ASIO是需要CPU模拟，还是设备本身和耳机接触不良，或者C口接触不良，总之会有音频闪断的问题；这种隐性问题直接给我卡住了，我总不能把市面上支持ASIO的小尾巴买来一个个测吧？

于是翻家里电子垃圾堆翻到一个乐之邦的MU1，不知道是之前买什么送的，查了查资料说支持UAC2.0，可能在共享模式/WASAPI独占模式延迟会低一些，但是不支持ASIO。然后想了想，朝着Codex许了个愿望，让它把整个osu!的音频Hook成WASAPI 独占模式。既然有这么多私服魔改客户端也好，McOsu也好都做到了，那其实是可行的吧？

对话了几轮，没想到Codex真的搓了个原型出来，接下来在主菜单卡了很久，一进主菜单动画、音乐就会变得特别快，进入选歌界面又正常；为了这个我把2026版本的osu!反编译了扔给Codex，又对话了二三十轮，排查了很久很久。

最后由于我观察到主菜单的音乐速度和帧数有关系，切后台锁30帧没那么快，把这个线索提供给Codex后，它终于定位到是主菜单由于要画频谱，会不断调用BASS_ChannelGetLevel 做可视化，需要Hook为从mixer缓冲读取，而不是直接读取decode channel。这样总算是修完了。

在这个项目里我起到的唯一作用是在大方向上指导技术可行性，反编译osu!来给Codex补充上下文，以及在Windows环境下编译程序并测试。所有的bass相关的资料、写代码都是Codex的GPT5.4老师完成的。

以往以天为单位，（如果非相关行业资深人员可能要以星期为单位）的资料查找、代码编写、排错，现在以分钟为单位快速推进，并且LLM只要充够Token就不知道苦不知道累，反编译出来没有类名变量名的C#代码照样能啃下去。

从来没有这么深刻的体会过原来我才是LLM的一个Tool。

点开Hook成功的屙屎，随便听着几首老歌，才明白可能有些东西真的回不去了。
