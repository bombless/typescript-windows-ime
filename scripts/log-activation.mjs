import { appendFileSync } from "node:fs";
import { resolve } from "node:path";

const rawEvent = process.argv[2] ?? process.env.IME_ACTIVATION_EVENT ?? "{}";
let activation = {};
try {
  activation = JSON.parse(rawEvent);
} catch {
  activation = { rawEvent };
}

const clientImage = activation.clientImage ?? null;
const appName = clientImage ? clientImage.split(/[/\\]/).pop() ?? null : null;

const record = {
  loggedAt: new Date().toISOString(),
  event: "ime-activate",
  session: activation.session,
  appName,
  clientPid: activation.clientPid ?? null,
  clientImage,
  activation,
};

appendFileSync(resolve(process.cwd(), "run.log"), JSON.stringify(record) + "\n", "utf8");
