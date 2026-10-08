import { startPipeServer, PIPE_NAME, type PipeServer } from "./pipe/server.js";
import { claimClientOwnership } from "./pipe/ownership.js";

const LISTEN_ATTEMPTS = 10;
const LISTEN_RETRY_MS = 500;

const ownership = claimClientOwnership();
let server: PipeServer | undefined;

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function ownPipe(): Promise<void> {
  for (let attempt = 1; attempt <= LISTEN_ATTEMPTS; attempt++) {
    const candidate = startPipeServer();
    try {
      await candidate.ready;
      server = candidate;
      return;
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      console.error(`[PIPE] attempt ${attempt}/${LISTEN_ATTEMPTS} could not own ${PIPE_NAME}: ${message}`);
      await candidate.close();
      await delay(LISTEN_RETRY_MS);
    }
  }
  console.error(`[PIPE] ${PIPE_NAME} stayed busy. Stop the other IME client (TypeScriptWindowsImeHost.exe or another dev process) and retry.`);
  ownership.release();
  process.exit(1);
}

void ownPipe();

const shutdown = () => {
  const closing = server ? server.close() : Promise.resolve();
  void closing.then(() => {
    ownership.release();
    process.exit(0);
  });
};

process.once("SIGINT", shutdown);
process.once("SIGTERM", shutdown);
process.once("exit", () => ownership.release());