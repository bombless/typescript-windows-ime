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

Native DLLs are split into build and install locations: `native\\build\\TypeScriptWindowsIme.dll` is produced by `build.ps1`, while `native\\install\\TypeScriptWindowsIme-current.dll` is the copy registered with Windows. `build.ps1` never touches the installed copy. If an older COM registration or TSF host still has the old DLL loaded, run `native\\cleanup-com.ps1` from an elevated PowerShell to unregister the project CLSID, stop common TSF hosts, and restart `ctfmon` before rebuilding.

The pipe Host follows the same split. `build.ps1` links
`native\\build\\TypeScriptWindowsImeHost.exe`; `native\\install-host.ps1` (also
`npm run host`) stops any running Host, copies the build into
`native\\install\\TypeScriptWindowsImeHost-<timestamp>.exe`, and starts it, so a
running Host never blocks `build.ps1` with LNK1104. Only one Host can run at a
time because of its singleton mutex.

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

## Minimal TSF registration skeleton

The native layer now contains a minimal keyboard TIP COM server. It implements `IClassFactory` and `ITfTextInputProcessor`, registers a stable development CLSID/profile, registers `GUID_TFCAT_TIP_KEYBOARD`, and exposes the standard COM self-registration exports. It does not handle key events yet.

Build the native DLL:

```powershell
.\\native\\build.ps1
```

Register it from an **elevated 64-bit PowerShell**:

```powershell
.\\native\\register.ps1
```

The matching uninstall command is:

```powershell
.\\native\\unregister.ps1
```

The profile is registered with LANGID `0xFFFF` (all languages); Windows Settings will only surface it where the corresponding language/profile can be used. This skeleton is intentionally separate from the PipeBridge and does not yet connect TSF key events to Node.js.
