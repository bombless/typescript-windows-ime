import { spawnSync } from "node:child_process";
import { existsSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

// A named pipe can only be owned by one process, so a second `npm run dev`
// silently loses every message: the stale client keeps answering the TSF DLL
// while the new one listens on a pipe nobody owns. The lock file records the
// owner so a fresh process can evict it instead.
export const DEFAULT_LOCK_PATH = join(tmpdir(), "typescript-windows-ime.client.lock");
const EVICTION_TIMEOUT_MS = 5000;

export interface ClientOwnership {
  lockPath: string;
  release(): void;
}

export function parseOwnerPid(contents: string): number | undefined {
  const value = Number.parseInt(contents.trim(), 10);
  return Number.isSafeInteger(value) && value > 0 ? value : undefined;
}

export function isProcessAlive(pid: number): boolean {
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    // EPERM means the process exists but belongs to another user.
    return (error as NodeJS.ErrnoException).code === "EPERM";
  }
}

export function killProcessTree(pid: number): boolean {
  // tsx runs the entry point in a child process, so the whole tree has to go.
  const result = spawnSync("taskkill", ["/PID", String(pid), "/T", "/F"], {
    stdio: "ignore",
    windowsHide: true,
  });
  return result.status === 0;
}

function sleepMs(milliseconds: number): void {
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, milliseconds);
}

export function waitForProcessExit(pid: number, timeoutMs: number): boolean {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (!isProcessAlive(pid)) return true;
    sleepMs(100);
  }
  return !isProcessAlive(pid);
}

export function claimClientOwnership(lockPath = DEFAULT_LOCK_PATH): ClientOwnership {
  if (existsSync(lockPath)) {
    const owner = parseOwnerPid(readFileSync(lockPath, "utf8"));
    if (owner !== undefined && owner !== process.pid && isProcessAlive(owner)) {
      console.log(`[PIPE] evicting previous client pid=${owner}`);
      if (killProcessTree(owner)) {
        const exited = waitForProcessExit(owner, EVICTION_TIMEOUT_MS);
        console.log(`[PIPE] previous client pid=${owner} exited=${exited}`);
      } else {
        console.log(`[PIPE] previous client pid=${owner} could not be terminated`);
      }
    }
  }
  writeFileSync(lockPath, String(process.pid), "utf8");

  return {
    lockPath,
    release() {
      try {
        if (parseOwnerPid(readFileSync(lockPath, "utf8")) === process.pid) {
          rmSync(lockPath, { force: true });
        }
      } catch {}
    },
  };
}