import { existsSync, readFileSync } from "node:fs";
import { resolve } from "node:path";
import { readScripts, writeScripts } from "./automation.js";

const [command, ...args] = process.argv.slice(2);

function usage(): never {
  console.log(`Usage: npm run cli -- <command>

Commands:
  history [--limit N]       Show recent TSF/pipe activity from ts-pipe.log
  scripts                  List scripts run when TSF activates
  add-script <name> <path> Register a local Node.js script for activation
  remove-script <name>     Remove an activation script

Activation scripts receive a JSON event as argv[2] and IME_ACTIVATION_EVENT.
The script path is trusted executable code; only register scripts you control.`);
  process.exit(0);
}

function history(): void {
  const path = resolve("ts-pipe.log");
  if (!existsSync(path)) {
    console.log("No ts-pipe.log found yet. Start the IME host/client and activate the input method first.");
    return;
  }
  const limitIndex = args.indexOf("--limit");
  const limit = limitIndex >= 0 ? Math.max(1, Number(args[limitIndex + 1]) || 100) : 100;
  const lines = readFileSync(path, "utf8").split(/\r?\n/).filter(Boolean);
  const matches = lines.filter((line) => /\[PIPE ← C\+\+\].*"type":"hello"/.test(line)
    || /\[PIPE ← C\+\+\].*"type":"keyDown"/.test(line)
    || /\[PIPE ← C\+\+\].*"type":"reset"/.test(line));
  console.log(matches.slice(-limit).join("\n") || "No matching TSF activity found.");
}

switch (command) {
  case "history":
    history();
    break;
  case "scripts":
    console.log(JSON.stringify(readScripts(), null, 2));
    break;
  case "add-script": {
    const [name, scriptPath] = args;
    if (!name || !scriptPath) usage();
    const entries = readScripts();
    if (entries.some((entry) => entry.name === name)) throw new Error(`Script already exists: ${name}`);
    entries.push({ name, path: resolve(scriptPath) });
    writeScripts(entries);
    console.log(`Registered "${name}". It will run on the next TSF activation.`);
    break;
  }
  case "remove-script": {
    const [name] = args;
    if (!name) usage();
    const entries = readScripts();
    const next = entries.filter((entry) => entry.name !== name);
    if (next.length === entries.length) throw new Error(`No script named "${name}"`);
    writeScripts(next);
    console.log(`Removed "${name}".`);
    break;
  }
  case undefined:
  case "--help":
  case "-h":
  case "help":
    usage();
    break;
  default:
    console.error(`Unknown command: ${command}`);
    usage();
}
