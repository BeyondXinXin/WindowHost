# WindowHost

WindowHost 把正在调试的 Windows 桌面程序主窗口放进固定 **1920 × 1080 物理像素客户区**。标题栏、菜单和边框另占空间。工具使用 C++17 和 Win32 API，可独立于 Qt 安装运行。

## 下载和使用

1. 每次推送后，GitHub Actions 使用 Visual Studio 2022 构建 x64 Release。登录 GitHub，打开构建成功的运行记录，在 **Artifacts** 中点击 `WindowHost.exe` 下载，然后在 Windows 10 1803 及以上版本或 Windows 11 上启动。[构建记录](https://github.com/BeyondXinXin/WindowHost/actions/workflows/build.yml)
2. 点击左上角的齿轮，在 **进程名** 中每行填写一个 EXE 名称（例如 `Navigation.exe`），点击 **保存**。匹配忽略大小写，适用于任意目录下的同名程序。
3. 照常在 Qt Creator 中启动调试。默认使用 **标签窗口**，点击齿轮右侧的标签切换目标，标签中的 PID 用于区分同名进程。在齿轮设置中选择 **独立窗口**，每个目标即可单独拖动和最小化。保存后布局立即生效。
4. 目标退出时，它的标签或多余的独立窗口随之关闭，最后一个空宿主继续等待。手动关闭宿主会释放其中所有目标，恢复其原有样式和位置。手动关闭最后一个宿主会退出 WindowHost。

设置保存在 `%LOCALAPPDATA%\BeyondXinXin\WindowHost\target.txt`。升级时，旧配置中的完整路径会自动按 EXE 文件名读取。

## 显示与调试行为

- 等待界面、菜单栏和进程名输入框随 Windows 的应用颜色模式切换，开启高对比度时使用系统配色。嵌入的程序自行控制配色。
- 在要调整的标签或独立窗口中打开齿轮，勾选 **启用任务栏**。底部留白只应用于该目标，高度按宿主所在显示器的实际工作区计算。例如高度为 48 像素时，目标区域为 1920 × 1032。设置按进程名和同名实例序号保存，同名的新实例使用当前最小的空闲序号。旧版的任务栏选项作为未单独配置的窗口的初始值。
- 宿主支持拖动、最小化和还原。跨不同缩放比例的显示器移动时，标题栏和菜单按显示器 DPI 调整，客户区保持固定像素数。要同时看全客户区和边框，显示器可用工作区须大于 1920 × 1080。
- 常规 Qt 全屏和无边框主窗口通过重新设置父窗口和客户区尺寸嵌入。输出布局由目标程序响应尺寸变化。带独立交换链的独占全屏、程序主动切换显示模式或内部写死渲染尺寸的情况，取决于目标实现。
- 每个进程托管一个主窗口。该进程有多个候选窗口时，优先选择可见、无所有者且客户区最大的窗口。目标自己的独立对话框和弹出菜单仍按应用原有方式显示。宿主和目标应处于同一交互桌面，并使用相同权限级别。
- Qt Creator 继续控制断点、单步和停止。宿主在工作线程中改变目标窗口样式和父窗口。Windows 会关联父子窗口的输入队列，目标在断点处暂停时，焦点切换或窗口释放可能需要等到继续运行或停止调试后完成。[Windows 跨线程父子窗口说明](https://devblogs.microsoft.com/oldnewthing/20130412-00/?p=4683)
- 宿主使用 Per-Monitor V2 DPI 感知，并为每个目标创建与其 DPI 上下文匹配的承载窗口。Windows 对 DPI 上下文不匹配的跨进程重设父窗口操作可能强制重置目标进程的 DPI 感知模式。[SetParent](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setparent) · [混合 DPI 托管](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setthreaddpihostingbehavior)
