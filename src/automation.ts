import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { resolve, isAbsolute } from "node:path";
import { spawn } from "node:child_process";

export interface ActivationEvent {
  type: "activate";
  session: number;
  pid: number;
  timestamp: string;
}

export interface ScriptEntry {
  name: string;
  path: string;
}

const configPath = () => resolve(process.cwd(), ".ime-scripts.json");

export function readScripts(): ScriptEntry[] {
  const path = configPath();
  if (!existsSync(path)) return [];
  const value: unknown = JSON.parse(readFileSync(path, "utf8"));
  if (!Array.isArray(value)) throw new Error(`${path} must contain a JSON array`);
  return value.map((item, index) => {
    if (!item || typeof item.name !== "string" || typeof item.path !== "string") {
      throw new Error(`Invalid script entry at index ${index}`);
    }
    return { name: item.name, path: item.path };
  });
}

export function writeScripts(entries: ScriptEntry[]): void {
  writeFileSync(configPath(), JSON.stringify(entries, null, 2) + "\n", "utf8");
}

export function runActivationScripts(event: ActivationEvent): void {
  for (const script of readScripts()) {
    const scriptPath = resolve(script.path);
    if (!isAbsolute(scriptPath) || !existsSync(scriptPath)) {
      console.error(`[automation] script "${script.name}" not found: ${scriptPath}`);
      continue;
    }
    // Scripts are explicitly user-installed local code. The event is passed as
    // one JSON argument so scripts don't need to scrape logs or guess the session.
    const child = spawn(process.execPath, [scriptPath, JSON.stringify(event)], {
      cwd: process.cwd(),
      stdio: "inherit",
      windowsHide: true,
      env: { ...process.env, IME_ACTIVATION_EVENT: JSON.stringify(event) },
    });
    child.on("error", (error) => console.error(`[automation] ${script.name}: ${error.message}`));
    child.on("exit", (code, signal) => {
      if (code !== 0) console.error(`[automation] ${script.name} exited code=${code} signal=${signal ?? "none"}`);
    });
  }
}
