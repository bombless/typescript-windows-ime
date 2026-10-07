import { startPipeServer } from "./pipe/server.js";

const server = startPipeServer();
const shutdown = () => server.close();
process.once("SIGINT", shutdown);
process.once("SIGTERM", shutdown);