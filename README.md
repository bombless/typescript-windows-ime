# TypeScript Windows IME

A Windows TSF input method with a thin native C++ adapter and a Node.js/TypeScript logic core.

See [PLAN.md](./PLAN.md) for the implementation plan and architecture.

## Architecture

Windows TSF → C++ native adapter / Named Pipe server → Node.js/TypeScript IME client

The first milestone is a reliable end-to-end key event round trip. Business logic stays in TypeScript; C++ is limited to Windows TSF/COM, composition, and IPC glue.

## Development

Requirements: Windows, Node.js 20+, and npm.

```powershell
npm install
npm test
npm run build
npm run dev
```

The IPC uses UTF-8 JSON Lines over two Windows Named Pipes. Start
`native\build\TypeScriptWindowsImeHost.exe` separately; Node connects to
`\\.\pipe\TypeScriptWindowsIME.Host`, while the TSF DLL connects to
`\\.\pipe\TypeScriptWindowsIME.Tsf`.

The native Host owns both Named Pipe servers and remains online independently
of TSF activation. The TypeScript process and TSF DLL connect as clients.

The pipe Host follows the same split. `build.ps1` links
`native\\build\\TypeScriptWindowsImeHost.exe`; `native\\install-host.ps1` (also
`npm run host`) stops any running Host, copies the build into
`native\\install\\TypeScriptWindowsImeHost-<timestamp>.exe`, and starts it, so a
running Host never blocks `build.ps1` with LNK1104.

Phase 0/1 deliberately keeps behavior small: alphabetic `keyDown` events produce a lowercase composition; the native adapter remains responsible for Windows TSF/COM concerns.

## Candidate window

`native\CandidateWindow.cpp` draws the candidate list itself. Most applications
(Notepad, browsers, editors) never render the `ITfCandidateListUIElement` that a
text service exposes, so the TIP owns a `WS_EX_NOACTIVATE | WS_EX_TOPMOST` popup
on the thread that activated it. The window is anchored to the composition caret,
which is read with `ITfContextView::GetTextExt` inside the composition edit
session, flipped above the caret when it would leave the monitor work area, and
hidden again when the composition ends or the text service deactivates. Key
handling, selection, and commit stay in TypeScript; the window is display only.

## One IME client at a time

A Windows named pipe has a single owner, so a stale `npm run dev` process keeps
answering the TSF DLL while a newer one silently listens on a pipe nobody owns.
`src\pipe\ownership.ts` records the owner pid in
`%TEMP%\typescript-windows-ime.client.lock`; a fresh process terminates the
recorded process tree with `taskkill /T /F` before listening, and releases the
lock on shutdown. `npm run dev` retries for five seconds when the pipe is still
busy and then exits with an explicit message instead of hanging.

## Native TSF build and installation

The native layer contains a keyboard TIP COM server and the named-pipe Host. Build the native DLL, Host, and diagnostic/test programs with:



```powershell
.\\native\\build.ps1
```

Install or update the TSF DLL from PowerShell; the script requests elevation when needed, installs a versioned DLL, and verifies the registration:

```powershell
.\\native\\install-user.ps1
```

Start or update the native pipe Host with:

```powershell
npm run host
```

If a stale TSF/COM process holds the DLL or registration needs recovery, run `.\\native\\cleanup-com.ps1` before rebuilding. It stops relevant host processes and removes the COM/TSF registration; by default it leaves `ctfmon` stopped so the DLL can be replaced. After rebuilding, run `.\native\install-user.ps1` to register the new DLL. The old `install.ps1`, `register.ps1`, and `unregister.ps1` entry points have been removed in favor of this version-aware workflow.
