# Windows 输入法托盘图标计划（TRAY）

## 目标

在 Windows 任务栏通知区域（系统托盘）显示输入法当前模式，至少区分：

- **中**：中文模式
- **英**：英文模式

图标/提示文字必须反映输入法实际生效的模式，而不是只反映 Node.js 最近一次返回值。该功能用于绕过 Windows 11 上 TSF 自定义语言栏项目可能不显示、位置不可控或行为不稳定的问题。

## 当前项目背景

- 项目：`D:\mcp-agent-workspace\typescript-windows-ime`
- 原生 TSF 组件：`native\TsIme.cpp`
- 原生宿主可能为：`TypeScriptWindowsImeHost.exe`（开始实现前先检查实际启动方式和进程生命周期）
- TSF 会调用 Node/Core 并收到包含 `mode` 的响应，例如 `chinese` / `english`。
- `native\TsIme.cpp` 近期增加了 Node 回复模式的诊断日志，并已有 TSF Language Bar item 的实验实现；不要假设该 Language Bar 实验已经能在任务栏稳定显示。
- 之前 `native\build\` 下的原生代码构建曾成功，但新增日志/语言栏改动尚未确认已安装并运行于实际注册的 DLL。
- 不要在未经用户确认的情况下运行需要管理员权限、修改系统注册表或重注册 TSF DLL 的安装脚本。

## 推荐架构

**优先评估由独立常驻进程负责托盘图标**，不要在 TSF DLL 中直接创建托盘图标。

原因：
- TSF DLL 可能被多个应用进程加载；每个进程创建托盘图标会导致重复图标、生命周期混乱。
- TSF DLL 生命周期与前台应用绑定，不适合作为系统托盘 UI 的唯一所有者。
- 托盘图标应有单一、稳定的所有者，负责图标创建、更新、重启恢复和点击交互。

先检查现有 `TypeScriptWindowsImeHost.exe` 是否是合适的常驻进程。如果它会随着应用/TSF 调用退出，则应考虑新增轻量独立 tray/helper 进程，并通过明确的 IPC 与现有 Core/Host 通信。不要未经检查就假定 Host 一定常驻。

## 状态流

预期流程：

1. Node/Core 计算或切换模式，模式值为 `chinese` 或 `english`。
2. 模式变更通过 IPC 通知托盘进程。
3. 托盘进程保存最新模式，并更新图标与 tooltip。
4. TSF 成功应用模式后，应尽可能反馈实际生效状态；UI 状态不能仅依赖未经验证的请求值。
5. 托盘进程启动或重启时，从权威状态源同步当前模式，避免默认图标长期与实际模式不一致。
6. 当前前台应用切换时，需要确认项目的模式语义：
   - 如果模式是全局共享，则显示全局当前模式；
   - 如果模式按应用/文档分别保存，则需要随焦点上下文更新，不能仅显示上一个应用的模式。

建议统一模式协议并校验值：
- `chinese`
- `english`

对缺失或未知模式不要静默地显示为其中一种；保留上次已确认状态，并记录诊断日志。

## Windows 实现建议

- 使用 Win32 Notification Area API：`Shell_NotifyIcon` / `NOTIFYICONDATA`。
- 优先使用稳定的 icon GUID；添加图标后设置 `NOTIFYICON_VERSION_4`。
- 提供适合 DPI 的图标资源（至少小尺寸和高 DPI 尺寸）。
- Tooltip 示例：
  - `TypeScript Windows IME — 中文模式`
  - `TypeScript Windows IME — English mode`
- 右键菜单至少可包含“当前模式”和“退出/重启诊断”等适合的操作；退出功能需避免误杀核心输入法服务。
- 左键点击切换模式可以作为第二阶段功能。若实现，必须向权威 Core/Host 请求切换，并等待确认后再更新图标；不能只在托盘 UI 本地翻转。
- 正确处理 Explorer 重启（如任务栏重启消息），重新创建托盘图标。
- 正确处理托盘进程退出、IPC 断连和重连。
- 托盘图标可能被 Windows 放进隐藏图标溢出区；这是 Windows 的用户可配置行为，不能承诺图标始终固定显示在主任务栏区域。
- 不把模式变化做成频繁的通知气泡；图标与 tooltip 即可表达持续状态。

## 实施前的代码勘察清单

在编写代码前先检查并记录：

- [ ] 项目入口、进程模型以及 `TypeScriptWindowsImeHost.exe` 的启动/退出方式。
- [ ] TSF DLL 与 Node/Core 之间现有的 IPC 协议、消息格式和连接生命周期。
- [ ] 当前模式状态的权威来源：Node/Core、Host，还是 TSF conversion compartment。
- [ ] 模式是全局共享还是按应用/文档保存。
- [ ] 现有 build 脚本、安装脚本、资源文件和打包流程。
- [ ] 是否已有单实例互斥、命名管道或其他 IPC 可复用。
- [ ] 现有 `TsIme.cpp` 中 Language Bar 实验代码的影响；不要让它与新托盘实现互相冲突。

## 建议分阶段交付

### Phase 1：只读状态托盘图标（MVP）

- [ ] 建立唯一的托盘图标所有者。
- [ ] 显示中/英两种不同且高对比度的图标。
- [ ] Tooltip 显示完整模式文本。
- [ ] 从权威状态源取得初始模式。
- [ ] 收到模式变更后更新图标与 tooltip。
- [ ] 增加清晰日志：启动、初始状态同步、模式更新、IPC 错误、图标添加/更新失败、退出。
- [ ] 在不提权的开发构建中验证可运行性。

### Phase 2：稳定性与前台上下文

- [ ] Explorer 重启后自动恢复图标。
- [ ] 托盘进程重启后重新同步模式。
- [ ] IPC 断开与重连不导致错误状态。
- [ ] 验证多个应用之间切换时显示正确。
- [ ] 验证多实例不会产生重复图标。

### Phase 3：托盘交互（可选）

- [ ] 左键或菜单命令请求切换中/英模式。
- [ ] 等待 Core/Host 确认后再改变 UI。
- [ ] 提供“当前模式”及适当的诊断菜单项。
- [ ] 如有设置项，尊重用户对图标可见性的控制。

## 验收标准

- [ ] 正常启动后只出现一个输入法托盘图标。
- [ ] 中文模式显示“中”图标，英文模式显示“英”图标，tooltip 一致。
- [ ] 通过现有输入法逻辑切换模式时，托盘及时更新。
- [ ] 切换前台应用后，显示值符合项目定义的全局/按应用模式语义。
- [ ] Node/Core/Host 重启或 IPC 暂时断开时，不会悄悄把未知状态当作已确认模式。
- [ ] Explorer 重启后图标可恢复。
- [ ] Debug 日志能追踪模式来源、同步、更新和失败原因。
- [ ] 构建与测试步骤可重复；未经确认不执行需要管理员权限的系统安装/注册步骤。

## 相关资料

- Microsoft — Notifications and the Notification Area：<https://learn.microsoft.com/en-us/windows/win32/shell/notification-area>
- Microsoft — Shell_NotifyIcon：<https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyicona>
- Microsoft — NotificationIcon SDK sample：<https://learn.microsoft.com/en-us/windows/win32/shell/samples-notificationicon>
- Microsoft — TSF Language Bar：<https://learn.microsoft.com/en-us/windows/win32/tsf/language-bar>
- Community project — IMEIndicator（供行为和设计参考，不代表必须依赖）：<https://github.com/Icecovery/IMEIndicator>

## 给下一个上下文的第一条指令

请先阅读本文件和项目当前工作树，检查进程模型、IPC 与模式状态的权威来源；然后提出最小改动方案。不要直接重写 TSF/Node 架构，不要丢弃现有未提交更改，也不要运行需要管理员权限的安装或注册脚本，除非用户明确授权。
