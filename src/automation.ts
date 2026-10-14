import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { resolve, isAbsolute, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { spawn } from "node:child_process";

export interface ActivationEvent { type: "activate"; session: number; pid: number; timestamp: string; clientPid?: number; clientImage?: string; }
export interface ScriptEntry { name: string; path: string; }

const configPath = () => resolve(process.cwd(), ".ime-scripts.json");
const builtinScriptPath = () => resolve(dirname(fileURLToPath(import.meta.url)), "../scripts/log-activation.mjs");

export function readScripts(): ScriptEntry[] {
  const path = configPath();
  const configured: ScriptEntry[] = [];
  if (existsSync(path)) {
    const value: unknown = JSON.parse(readFileSync(path, "utf8"));
    if (!Array.isArray(value)) throw new Error(path + " must contain a JSON array");
    configured.push(...value.map((item, index) => {
      if (!item || typeof item.name !== "string" || typeof item.path !== "string") throw new Error("Invalid script entry at index " + index);
      return { name: item.name, path: item.path };
    }));
  }
  // Built-in logger runs on every activation without manual registration.
  const builtin = { name: "log-activation", path: builtinScriptPath() };
  return [builtin, ...configured.filter((entry) => entry.name !== builtin.name)];
}

export function writeScripts(entries: ScriptEntry[]): void {
  writeFileSync(configPath(), JSON.stringify(entries.filter((entry) => entry.name !== "log-activation"), null, 2) + "\n", "utf8");
}

export function runActivationScripts(event: ActivationEvent): void {
  for (const script of readScripts()) {
    const scriptPath = resolve(script.path);
    if (!isAbsolute(scriptPath) || !existsSync(scriptPath)) {
      console.error("[automation] script \"" + script.name + "\" not found: " + scriptPath);
      continue;
    }
    const child = spawn(process.execPath, [scriptPath, JSON.stringify(event)], {
      cwd: process.cwd(), stdio: "inherit", windowsHide: true,
      env: { ...process.env, IME_ACTIVATION_EVENT: JSON.stringify(event) },
    });
    child.on("error", (error) => console.error("[automation] " + script.name + ": " + error.message));
    child.on("exit", (code, signal) => {
      if (code !== 0) console.error("[automation] " + script.name + " exited code=" + code + " signal=" + (signal ?? "none"));
    });
  }
}