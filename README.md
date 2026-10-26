# dsh-task-pet

在浏览器之外常驻桌面的 dsh 桌宠：实时显示当前会话的 todo 进度、进行中的任务和等待确认状态。

- 透明、置顶、可拖拽的小窗口，常驻桌面右下角
- 蓝色 = 工作中，橙色 = 等待确认，绿色 = 空闲
- 外圈进度环和 `3/7` 角标；悬停显示任务气泡
- 点击打开 dsh，右键可刷新或退出
- Win32 + GDI+，静态链接，无需任何运行时，常驻内存约 14 MB

## 工作方式

1. `index.js` 是 dsh Host 插件，监听 `session/event`（`todo/write`、`approval/asked|decided`）和 `agent/status`
2. 插件把当前会话压缩成几行状态写到 `~/.dsh/dsh-pet/state.txt`（原子替换）
3. `bin/DshPet.exe` 每 200ms 检查状态文件，变化即重绘；dsh 退出后 6 秒自动关闭

## 构建桌宠

需要 [Zig](https://ziglang.org/)（用 `zig c++` 编译，无需 Visual Studio）：

```powershell
.\build.ps1 -Zig C:\path\to\zig.exe
```

产物为 `bin/DshPet.exe`，可直接替换仓库里的二进制。

## 安装插件

推荐让 dsh 的 Agent 执行 `plugin_manager` 的 `install_bundle`，目标为当前目录。
手动安装：

```powershell
dsh plugin --profile web add file:C:/path/to/dsh-task-pet
```

然后把 `dsh-task-pet` 加入 profile `package.json` 的 `dsh.profile.bundles` 并重启 dsh。

## 配置

| 环境变量 | 默认值 | 说明 |
| --- | --- | --- |
| `DSH_PET_STATE` | `~/.dsh/dsh-pet/state.txt` | 状态文件路径 |
| `DSH_PET_URL` | `http://127.0.0.1:3080/` | 点击宠物打开的地址 |

窗口位置保存在状态文件同目录的 `pos.txt`。
