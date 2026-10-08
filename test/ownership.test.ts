import { describe, expect, it } from "vitest";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { claimClientOwnership, isProcessAlive, parseOwnerPid } from "../src/pipe/ownership.js";

function lockPath(): string {
  return join(mkdtempSync(join(tmpdir(), "ime-ownership-")), "client.lock");
}

describe("client ownership", () => {
  it("reads the pid written by a previous client", () => {
    expect(parseOwnerPid("4321\n")).toBe(4321);
    expect(parseOwnerPid("not-a-pid")).toBeUndefined();
    expect(parseOwnerPid("")).toBeUndefined();
  });

  it("records this process as the pipe owner", () => {
    const path = lockPath();
    const ownership = claimClientOwnership(path);
    expect(readFileSync(path, "utf8")).toBe(String(process.pid));
    expect(isProcessAlive(process.pid)).toBe(true);
    ownership.release();
    expect(existsSync(path)).toBe(false);
    rmSync(join(path, ".."), { recursive: true, force: true });
  });

  it("takes over a lock left behind by a dead client", () => {
    const path = lockPath();
    // A pid that cannot exist keeps the takeover path free of real kills.
    writeFileSync(path, "4294967294", "utf8");
    const ownership = claimClientOwnership(path);
    expect(readFileSync(path, "utf8")).toBe(String(process.pid));
    ownership.release();
    expect(existsSync(path)).toBe(false);
    rmSync(join(path, ".."), { recursive: true, force: true });
  });

  it("keeps the lock when another client already owns it", () => {
    const path = lockPath();
    writeFileSync(path, String(process.pid), "utf8");
    const ownership = claimClientOwnership(path);
    expect(readFileSync(path, "utf8")).toBe(String(process.pid));
    rmSync(join(path, ".."), { recursive: true, force: true });
    expect(() => ownership.release()).not.toThrow();
  });
});