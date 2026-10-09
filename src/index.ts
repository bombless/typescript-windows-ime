import { startPipeClient, PIPE_NAME, type PipeClient } from "./pipe/client.js";

let client: PipeClient | undefined;
let shuttingDown = false;
function log(message: string, error = false): void {
  const now = new Date();
  const pad = (n: number, width = 2) => String(n).padStart(width, "0");
  const stamp = `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())} ${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())}.${pad(now.getMilliseconds(), 3)}`;
  (error ? console.error : console.log)(`[${stamp}][pid=${process.pid}] ${message}`);
}

function shutdown(code = 0): void {
  if (shuttingDown) return;
  shuttingDown = true;
  client?.close();
  process.exitCode = code;
}

client = startPipeClient(PIPE_NAME, {
  onConnected: () => log(`[PIPE] connected to Host: ${PIPE_NAME}`),
  onUnavailable: (error) => {
    log(`[PIPE] Host unavailable at ${PIPE_NAME}: ${error.message}`, true);
    shutdown(1);
  },
  onDisconnected: () => {
    if (!shuttingDown) {
      log("[PIPE] Host closed the business-client connection; exiting this client.", true);
      shutdown(0);
    }
  },
});

process.once("SIGINT", () => shutdown(0));
process.once("SIGTERM", () => shutdown(0));