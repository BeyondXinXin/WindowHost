# WindowHost

WindowHost 把正在调试的 Windows 桌面程序主窗口放进固定 **1920 × 1080 物理像素客户区**。标题栏、菜单和边框另占空间。工具使用 C++17 和 Win32 API，可独立于 Qt 安装运行。

## 下载和使用

1. 每次推送后，GitHub Actions 使用 Visual Studio 2022 构建 x64 Release。登录 GitHub，打开构建成功的运行记录，在 **Artifacts** 中点击 `WindowHost.exe` 下载，然后在 Windows 10 1803 及以上版本或 Windows 11 上启动。[构建记录](https://github.com/BeyondXinXin/WindowHost/actions/workflows/build.yml)
2. 打开 **设置**，每行输入一个进程名（例如 `Navigation.exe`），点击 **保存**。匹配忽略大小写，适用于任意目录下的同名程序。
3. 照常在 Qt Creator 中启动调试。每个匹配的进程各用一个独立宿主窗口，标题显示进程名和 PID。同名程序启动多份时也会分别托管。
4. 目标进程退出时，多余的宿主窗口自动关闭，最后一个窗口继续等待。手动关闭某个宿主会释放对应目标，恢复其原有样式和位置。该进程重新启动后可以再次捕获。手动关闭最后一个宿主会退出 WindowHost。

设置保存在 `%LOCALAPPDATA%\BeyondXinXin\WindowHost\target.txt`。升级时，旧配置中的完整路径会自动按 EXE 文件名读取。

## 显示与调试行为

- 等待界面、菜单栏和进程名输入框随 Windows 的应用颜色模式切换，开启高对比度时使用系统配色。嵌入的程序自行控制配色。
- 在设置中勾选 **底部预留当前显示器的任务栏高度**，可在每个宿主底部留出一段空白。高度取自所在显示器底边与可用工作区底边的物理像素差。比如底部预留 48 像素时，目标区域为 1920 × 1032。换屏或工作区变化时自动更新，底部没有预留空间时为 0。默认关闭。
- 宿主支持拖动、最小化和还原。跨不同缩放比例的显示器移动时，标题栏和菜单按显示器 DPI 调整，客户区保持固定像素数。要同时看全客户区和边框，显示器可用工作区须大于 1920 × 1080。
- 常规 Qt 全屏和无边框主窗口通过重新设置父窗口和客户区尺寸嵌入。输出布局由目标程序响应尺寸变化。带独立交换链的独占全屏、程序主动切换显示模式或内部写死渲染尺寸的情况，取决于目标实现。
- 每个进程托管一个主窗口。该进程有多个候选窗口时，优先选择可见、无所有者且客户区最大的窗口。目标自己的独立对话框和弹出菜单仍按应用原有方式显示。宿主和目标应处于同一交互桌面，并使用相同权限级别。
- Qt Creator 继续控制断点、单步和停止。宿主在工作线程中改变目标窗口样式和父窗口。Windows 会关联父子窗口的输入队列，目标在断点处暂停时，焦点切换或窗口释放可能需要等到继续运行或停止调试后完成。[Windows 跨线程父子窗口说明](https://devblogs.microsoft.com/oldnewthing/20130412-00/?p=4683)
- 宿主使用 Per-Monitor V2 DPI 感知，并为每个目标创建与其 DPI 上下文匹配的承载窗口。Windows 对 DPI 上下文不匹配的跨进程重设父窗口操作可能强制重置目标进程的 DPI 感知模式。[SetParent](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setparent) · [混合 DPI 托管](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setthreaddpihostingbehavior)
