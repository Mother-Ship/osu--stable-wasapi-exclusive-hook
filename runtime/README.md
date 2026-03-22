将构建得到的 `launcher.exe`、`osu_audio_hook.dll`、`bassmix.dll` 和
`basswasapi.dll` 放到此目录中。

启动器会从这个目录解析 Hook DLL，Hook 也会从同一位置解析这两个 BASS
附加模块。
