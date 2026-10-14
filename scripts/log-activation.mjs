import { appendFileSync } from "node:fs";
import { resolve } from "node:path";

const rawEvent = process.argv[2] ?? process.env.IME_ACTIVATION_EVENT ?? "{}";
let activation = {};
try {
  activation = JSON.parse(rawEvent);
} catch {
  activation = { rawEvent };
}

const record = {
  loggedAt: new Date().toISOString(),
  event: "ime-activate",
  activation,
  process: {
    pid: process.pid,
    ppid: process.ppid,
    executable: process.execPath,
    argv: process.argv,
    cwd: process.cwd(),
  },
};

appendFileSync(resolve(process.cwd(), "run.log"), JSON.stringify(record) + "\n", "utf8");
