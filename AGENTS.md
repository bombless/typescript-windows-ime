# Repository Guidelines

## Project Structure & Module Organization

The TypeScript runtime lives in src/: ime/ contains state, reduction, candidates, and engine logic; protocol/ defines JSONL messages and codecs; and pipe/ contains the named-pipe server. Tests are in test/ and use matching *.test.ts names. The Windows-native TSF/COM adapter and pipe bridge are in native/; its PowerShell scripts build, register, unregister, and clean up the adapter. The pinyin dictionary is under data/. Generated JavaScript goes to dist/ and should not be edited manually.

## Build, Test, and Development Commands

Use Node.js 20+ on Windows:

- npm install — install dependencies.
- npm test — run the Vitest suite once.
- npm run test:watch — run Vitest interactively while developing.
- npm run build — compile strict TypeScript to dist/.
- npm run dev — run the TypeScript server through tsx.
- npm start — run the compiled server.
- .\native\build.ps1 — build the native DLL/host; registration scripts require elevated 64-bit PowerShell.

For an end-to-end run, start the native host and Node process separately as described in README.md; components communicate with UTF-8 JSON Lines over Windows named pipes.

## Coding Style & Naming Conventions

Follow strict TypeScript settings: ES modules, two-space indentation, semicolons, and double-quoted strings. Use camelCase for variables/functions, PascalCase for classes/types, and descriptive message discriminators such as keyDown. Keep IME business logic in TypeScript; limit C++ changes to TSF/COM and IPC responsibilities. Preserve explicit .js extensions in TypeScript imports. Run npm run build before submitting changes.

## Testing Guidelines

Vitest is the test framework. Add focused tests in test/ using describe/it names that state observed behavior. Protocol tests should cover malformed and fragmented JSONL as well as round trips; IME tests should cover state transitions and consumption. Run npm test and keep tests independent of Windows TSF where possible.

## Commit & Pull Request Guidelines

Recent commits use short imperative or action-oriented subjects, often concise Chinese, such as 修复 and 接入小狼毫. Keep commits focused and explain the user-visible or architectural effect. Pull requests should describe behavior changes, list validation commands and native prerequisites, link related issues when applicable, and include screenshots or reproduction steps for Windows/TSF-visible changes.

## Security & Configuration Tips

Treat named-pipe input as untrusted: preserve validation and bounded message handling. Do not commit credentials, generated binaries, or installed DLL copies. If a stale COM/TSF process holds a DLL, use native\cleanup-com.ps1 before rebuilding.
