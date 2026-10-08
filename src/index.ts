import { startPipeClient, PIPE_NAME, type PipeClient } from "./pipe/client.js";

let client: PipeClient | undefined;
let shuttingDown = false;

function shutdown(code = 0): void {
  if (shuttingDown) return;
  shuttingDown = true;
  client?.close();
  process.exitCode = code;
}

client = startPipeClient(PIPE_NAME, {
  onConnected: () => console.log(`[PIPE] connected to Host: ${PIPE_NAME}`),
  onUnavailable: (error) => {
    console.error(`[PIPE] Host unavailable at ${PIPE_NAME}: ${error.message}`);
    shutdown(1);
  },
  onDisconnected: () => {
    if (!shuttingDown) {
      console.error("[PIPE] Host closed the business-client connection; exiting this client.");
      shutdown(0);
    }
  },
});

process.once("SIGINT", () => shutdown(0));
process.once("SIGTERM", () => shutdown(0));