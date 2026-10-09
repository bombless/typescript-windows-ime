# PLAN — 把候选框渲染搬到 Host 进程


## 1. 目标

让候选框的渲染逻辑可以**热更新**：改一行 C++ 绘制代码后只重启 Host 进程，
所有正在运行的 app（Firefox、OpenCode、Notepad……）立刻生效，不需要重启它们。

## 2. 为什么现在做不到

`native/CandidateWindow.cpp`（501 行）本身已经完全解耦，只依赖
`CandidateWindow.h`，与 TSF 的唯一耦合是一个 `CaretProvider` 回调。阻塞热更新的
不是代码结构，而是链接方式：

```powershell
# native/build.ps1 当前
cl.exe /LD TsIme.cpp "$out\CandidateWindow.obj" "$out\PipeBridge.obj" ...
```

`CandidateWindow.obj` 被**静态链接进 `TypeScriptWindowsIme.dll`**，而这个 DLL 被
TSF 加载进每一个 app 进程且永不卸载。因此每次改动都要重启所有加载过它的进程。

把渲染放进 Host（单例、常驻、独立进程）就绕开了这个限制：Host 是唯一持有这份
代码的进程，重启它不影响任何 app。

## 3. 现状（已完成，不要重做）

架构与链路已经稳定，直接在此基础上改：

```text
app 进程                        Host (单例)                  Node (单例)
──────────                      ───────────                  ──────────
TsIme.dll
  ITfKeyEventSink
  ITfTextInputProcessor
  RequestEditSession ──caret──┐
                              │
  PipeBridge (client) ──JSONL──► ListenForTsfClients   ──JSONL──► SimpleEngine
        ▲                       map<sessionId, HANDLE>            │
        │                       ListenForNodeClient ◄──────────────┘
        └──── 按 sessionId 路由 ──┘  DispatchNodeToTsf
```

已修好、不要动的东西：

- **session 路由**。TSF 每个 app 进程一个 text service，请求 id 都从 1 开始，
  所以每条 JSONL 都带 `session`（`(pid << 20) | counter`），Host 按 session
  路由，Node 按 session 保存 IME 状态（`Map<sessionId, ImeState>`）。
- **Host 对管道独占**。启动时 `CreateFileW` 探测两条管道，被占用就退出码 1。
- **Host 全部管道用 `FILE_FLAG_OVERLAPPED`**。这是必须的：实测同一个命名管道
  实例上，一个线程阻塞在同步 `ReadFile` 时，另一个线程的 `WriteFile` 会**永久
  阻塞**。同步 I/O 方案已验证必死。
- **Host 侧 TSF 不做 newest-wins**，所有 session 都保留；只有 Node 客户端
  newest-wins（旧的靠管道断开自己退出）。
- 协议 `PROTOCOL_VERSION = 2`。

### 需要特别注意的一行代码

`native/CandidateWindow.cpp:425`：

```cpp
UINT dpi = GetDpiForWindow(window);
```

窗口在 app 进程里时，`GetDpiForWindow` 返回的是 **app 的** DPI。搬到 Host 之后
它返回的是 **Host 的** DPI，混合 DPI 显示器上字体会错。这是本次改动最容易漏的坑，
见 §5.1。

## 4. 改动后的架构

```text
app 进程                                Host
──────────                              ─────
TsIme.dll (不再含 CandidateWindow.obj)
  ITfKeyEventSink                       ListenForTsfClients
  ITfTextInputProcessor                   ├─ 按 sessionId 解析
  RequestEditSession ──caret──┐            ├─ showCandidates/hideCandidates → 本地渲染
                              │            └─ 其他消息 ──► Node (单连接)
  PipeBridge (client) ────────┴──JSONL──► ListenForNodeClient
                                                  │
                                     DispatchNodeToTsf ◄── Node
```

C++ 侧只负责：算 caret 矩形、拿候选列表、发消息。**不碰任何 GDI/窗口代码。**

## 5. 关键设计决策

### 5.1 DPI 必须由 C++ 传过来

`showCandidates` 消息里带 `dpi`，由 C++ 用
`GetDpiForWindow(GetForegroundWindow())` 得到，Host **原样使用**，不自己重新推导。

理由：TSF 报的 caret 矩形本来就是 app 坐标系转成的屏幕坐标，字号/行高也应该用
app 的 DPI。Host 自己算会在混合 DPI 下错位，而且对 DPI-unaware 的 app
（`CandidateWindowProbe` 的 `--dpi-unaware` / `unaware-host` 两个场景就是为这个
存在的）会退化成系统 DPI，改变现有行为。

### 5.2 caret 重试必须留在 app 进程

现在重试是 `CandidateWindow` 给自己 `SetTimer`（`kCaretRetryTimer`，
40ms × 12 次），`WindowProc` 里回调 `g_caretProvider` → `ReadCaretRectFromService`
→ `ProbeCaretRect` → `RequestEditSession(TF_ES_SYNC)`。

**因为窗口建在 TSF 回调线程上，这段代码今天跑在 app 的 UI 线程。** 搬到 Host 后
有两个选择：

| 方案 | 做法 | 代价 |
|---|---|---|
| **B（推荐）** | 定时器搬到 `TsIme.cpp`：app 进程里建一个 `HWND_MESSAGE` 消息窗口，重试时重新算 caret 并**重发** `showCandidates` | `CandidateWindow` 去掉定时器和 `SetCaretProvider`，`CandidateWindowProbe` 里用 `RetryCaretProvider` 的场景要跟着改 |
| A | 定时器留在 Host，Host 反向问 app 要 caret | `RequestEditSession` 变成在 `PipeBridge` 读线程上跨线程调用，IME 里这是真实的风险点 |

选 B：保持 `RequestEditSession` 在 UI 线程的现有语义不变，是行为等价而非行为变更。
代价只是改 probe 的几个场景，比引入跨线程 edit session 划算。

`CandidateWindowProbe.cpp` 里有多个场景用 `SetCaretProvider(&RetryCaretProvider)`
模拟重试（`RetryCaretRect` 等），方案 B 下要改成直接调 `Show` 传不同 caret 值。
probe 是这次改动的回归网，值得花这个时间。

### 5.3 `CaretFallback()` 必须删掉

`native/CandidateWindow.cpp:330`：

```cpp
if (ownerPid == GetCurrentProcessId()) { ... }
```

在 Host 里 `GetCurrentProcessId()` 永远是 Host 的 pid，这个分支**永远不成立**，
静默失效。要么删掉，要么让 C++ 把线程 caret 矩形一起发过来。

建议：删掉，靠 §5.2 的定时器重试覆盖；Host 拿不到 caret 时退回 `ApplicationRect()`
（它用 `GetForegroundWindow()`，跨进程调用，在 Host 里仍然有效）。

删掉之后要专门验证一次「app 迟迟不给 caret」的场景，确认仍然能在 app 窗口内
显示而不是不显示。

### 5.4 同步重绘改为异步，可以接受

现在 `Show` 里注释写明要同步重绘：

> Repaint synchronously: the key handler must not return before the list is on
> screen, and some hosts keep their message loop busy afterwards.

跨进程之后 `HandleKey` 写完管道就返回，Host 在自己的消息循环里画（通常一帧内）。
原注释担心的「host 消息循环忙」反而不再成立——**忙的是 app，不是 Host**。所以
这里异步是改进不是退化，但要在 Notepad / Firefox / OpenCode 上各确认一次视觉无差异。

### 5.5 不抢焦点

`WS_EX_NOACTIVATE` 已经在 `EnsureWindow` 里（`CandidateWindow.cpp:312`），
保持不变。另外注意它刻意**不设 owner**：owned popup 会被压在 owner 的 topmost
sibling 之下，而 owner 是别的 app 的窗口。跨进程之后这条理由依然成立。

### 5.6 窗口归属

同一时刻只有一个 app 有活动 composition，所以 Host 只维护一个窗口，但要记住
`g_renderOwnerSession`：

- 收到不同 session 的 `showCandidates` → 接管，旧 session 作废。
- 某个 session 从 `g_tsfClients` 里被移除（`ServeTsfClient` 退出时）→ 如果它
  是 owner，立刻隐藏窗口，否则会留下一个永远没人关的框。

### 5.7 消息不走 Node

`showCandidates` / `hideCandidates` 是 C++ → Host 的单向通知，**不要转发给 Node**。
Node 已经产出过候选列表了，再发一遍只是噪音，也会打乱 Host↔Node 纯请求/响应的
约定。Host 解析 `session` 后自己消费。

## 6. 实施步骤

### 步骤 1：协议加两个消息类型

`src/protocol/messages.ts`：新增

```ts
export interface ShowCandidatesMessage extends SessionScoped {
  id: number;
  type: "showCandidates";
  candidates: string[];
  selection: number;
  caret: { left: number; top: number; right: number; bottom: number };
  dpi: number;
}
export interface HideCandidatesMessage extends SessionScoped {
  id: number;
  type: "hideCandidates";
}
```

把它们加入 `RequestMessage` 联合类型。注意 `isRequestMessage` 现在对所有消息
都要求 `session >= 0`，新类型天然满足。

`PROTOCOL_VERSION` 升到 **3**，并同步更新 `test/protocol.test.ts` 里的版本字面量。

### 步骤 2：Host 链接 CandidateWindow 并加渲染分发

`native/build.ps1`：

- Host 那行加上 `"$out\CandidateWindow.obj"` 和 `gdi32.lib`（Host 现在只链
  `advapi32.lib`；`CandidateWindow.cpp` 用 `GetDC`/`CreateFontIndirect`/`DrawText`，
  需要 `gdi32`，还需要 `user32`——原来 DLL 那边链的，Host 这边要补）。
- DLL 那行**去掉** `"$out\CandidateWindow.obj"`。

`native/TypeScriptWindowsImeHost.cpp`：在 `ServeTsfClient` 的解析处分流——
读出 `type`，若是 `showCandidates`/`hideCandidates` 就调渲染并 `continue`，
不转发 Node。

渲染要跑在 Host 的 UI 线程上（窗口消息循环），所以 Host 需要一个消息循环线程；
最简单是渲染线程自己 `CreateWindowEx` 一个隐藏窗口并直接 `Show`（`CandidateWindow`
内部已经用 `SetWindowPos(SWP_SHOWWINDOW)` + `ShowWindow` + `RedrawWindow` 同步画完，
不依赖消息循环），但 `SetTimer` 重试已经按 §5.2 移走，所以**不需要**消息循环，
直接在 relay 线程调用 `CandidateWindow::Show` 即可。

### 步骤 3：改 CandidateWindow 的 API

`native/CandidateWindow.h`：

- `Show(...)` 增加 `UINT dpi` 参数（或加一个 `SetDpiForNextShow`）。
- 删除 `SetCaretProvider`、`CaretProvider` 类型和定时器相关成员。
- 删除 `CaretFallback()`。

`native/CandidateWindow.cpp`：

- `Show` 里把 `GetDpiForWindow(window)` 换成传入的 `dpi`。
- 删掉 `g_caretProvider` / `g_caretRetriesLeft` / `g_waitingForCaret` /
  `kCaretRetryTimer` / `kCaretRetries` / `kCaretRetryDelayMs` 和 `WindowProc`
  里的 `WM_TIMER` 分支。

### 步骤 4：TsIme.cpp 改成发消息

- `UpdateCandidates()` 不再调 `CandidateWindow::Show`，改成构造 `showCandidates`
  发给 Host。Caret 矩形由 `session.CaretRect()` 提供（已经是屏幕坐标）。
- `HideCandidates()` 发 `hideCandidates`。
- `TextService::Activate` / `Deactivate` 里的 `SetCaretProvider` /
  `CandidateWindow::Destroy()` 换成创建/销毁 §5.2 的消息窗口。
- `g_activeService` 和 `ReadCaretRectFromService` 保留（重试仍然需要它们）。
- 新增 app 侧重试：消息窗口的 `WM_TIMER` 里调 `ProbeCaretRect`，拿到新矩形就
  **重发** `showCandidates`。重试次数/间隔沿用 `kCaretRetries=12`、
  `kCaretRetryDelayMs=40`。

`PipeBridge` 需要一个发通知的方法（fire-and-forget，不等响应）。现有的
`Call()` 是严格请求/响应，加一个 `Notify(requestJson)`：只 `WriteAll`，不 `ReadLine`。
**注意**：不要在同一管道上让通知和响应交错时读到错位的响应——`Call` 已经有
`callCoreMutex_` 保护，通知也要走同一把锁。

### 步骤 5：更新 probe

`native/CandidateWindowProbe.cpp`：把 `SetCaretProvider(&RetryCaretProvider)`
改成直接用不同 caret 值调 `Show`，并适配新的 `dpi` 参数。跑一遍全部场景，
确认都还 PASS。

### 步骤 6：清理 TEMPORARY 代码

`native/CandidateWindow.cpp:371` 有一个临时的全屏宽度定位 hack：

```cpp
// TEMPORARY VISIBILITY TEST: cover the full monitor width from the caret
// down to the physical bottom edge.
const int width = workArea.right - workArea.left;
```

这正是搬进 Host 后最需要反复调的地方，顺手改回按 `Measure` 出来的实际尺寸。

## 7. 验收标准

1. `npm test` 通过，`npm run build` 无错，`.\native\build.ps1` 无 warning 级别的
   新问题。
2. `CandidateWindowProbe.exe` 全部场景 PASS（证明共享的 `CandidateWindow.cpp`
   没被改坏）。
3. Notepad / Firefox / OpenCode 三个 app 里都能正常显示候选框，位置、字号、
   高亮与改动前一致。
4. **热更新本身**（这是本计划的核心验收项）：
   - 打开 Firefox 和 OpenCode，不要重启。
   - 改 `CandidateWindow.cpp` 里的一处可见行为（比如高亮颜色）。
   - `.\native\build.ps1` + `.\native\install-host.ps1`。
   - 回到 Firefox 里打字 → 新行为立刻出现，Firefox 没重启。
5. Node 进程不在时，候选框不出现且普通键盘输入完全正常（fail-open 原则不变）。
6. app 切换 / 多显示器 / 混合 DPI 下，候选框仍贴在正确的 caret 下方。

## 8. 明确不做

- 不改 `TsIme.cpp` 里的 TSF/COM 部分。改那一层仍然需要重启 app，这次不解决。
- 不做 Electron / Web 渲染。IME 对按键延迟敏感，跨进程 + IPC + 合成的开销
  不适合进生产链路。
- 不拆成 `LoadLibrary`/`FreeLibrary` 的按需加载 DLL。收益远小于本方案，还要
  手工维护导出表和卸载顺序。
- 不引入新的 IPC 层。复用现有的两条管道。

## 9. 相关文件速查

| 文件 | 本次要改什么 |
|---|---|
| `native/CandidateWindow.h` / `.cpp` | 换 DPI 来源、删重试定时器、删 `CaretFallback` |
| `native/CandidateWindowProbe.cpp` | 适配新 API，回归网 |
| `native/TsIme.cpp` | `UpdateCandidates`/`HideCandidates` 改发消息；新增 app 侧重试窗口 |
| `native/PipeBridge.h` / `.cpp` | 新增 fire-and-forget 的 `Notify()` |
| `native/TypeScriptWindowsImeHost.cpp` | 分流渲染消息、链接 `CandidateWindow.obj` |
| `native/build.ps1` | Host 链 `CandidateWindow.obj` + `gdi32/user32`；DLL 去掉 |
| `src/protocol/messages.ts` | 新增两种消息，版本升 3 |
| `test/protocol.test.ts` | 新增消息的往返测试，更新版本字面量 |
