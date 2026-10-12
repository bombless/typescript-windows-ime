# CLI and TSF activation scripts

The CLI is a local management interface for the IME runtime.

```powershell
npm run cli -- history --limit 200
npm run cli -- scripts
npm run cli -- add-script example .\scripts\on-ime-activate.mjs
npm run cli -- remove-script example
```

Activity history is read from `ts-pipe.log`, which the Node pipe client already writes. It shows protocol events and session IDs. The current protocol does not yet provide a reliable target application name/process identity, so history cannot truthfully label a session as Firefox, Notepad, etc.; that requires adding foreground-process metadata to the native TSF activation message.

Activation scripts are listed in the local `.ime-scripts.json` file (keep it out of source control if it contains personal paths). Each registered script is started with the current Node executable when the TSF client receives a `hello` message. The activation event is provided as JSON in `process.argv[2]` and `IME_ACTIVATION_EVENT`, with `type`, `session`, `pid` (the Node runtime PID), and `timestamp`.

Example script:

```js
const event = JSON.parse(process.argv[2]);
console.log("IME activation", event.session, event.timestamp);
```

Only register scripts you trust: they run with the same permissions as the IME process. Scripts run asynchronously so they do not block the TSF request/response path. The initial implementation runs scripts on each protocol `hello`; native activation/deactivation notifications and target-application identity are follow-up protocol work.
