import { readFile } from 'node:fs/promises';
import { createAuthClient } from './authClient.js';
import { systemClock } from './clock.js';
import { loadConfig, type Config } from './config.js';
import { migrate } from './db/migrate.js';
import { adminDbOptions, createPool, worldDbOptions } from './db/pool.js';
import { createApp } from './http/app.js';
import { createWorldAdminClient } from './worldAdmin.js';

const log = console;

let config: Config;
try {
  config = loadConfig();
} catch (err) {
  log.error((err as Error).message);
  process.exit(1);
}

await migrate(adminDbOptions(config));
const adminPool = createPool(adminDbOptions(config));
const worldPool = createPool(worldDbOptions(config));
const world = createWorldAdminClient({
  secret: config.WORLD_ADMIN_SECRET,
  ca: config.WORLD_ADMIN_CA_FILE ? await readFile(config.WORLD_ADMIN_CA_FILE) : undefined,
  timeoutMs: config.WORLD_ADMIN_TIMEOUT_MS,
  log: (msg) => log.warn(msg),
});

const app = createApp({
  config,
  adminPool,
  worldPool,
  clock: systemClock,
  auth: createAuthClient(config.AUTH_URL),
  world,
  log,
});
const server = app.listen(config.PORT, () => log.info(`chthonic-admin listening on :${config.PORT}`));

for (const signal of ['SIGTERM', 'SIGINT'] as const) {
  process.once(signal, () => {
    log.info(`${signal} received, shutting down`);
    server.close(() => void Promise.all([adminPool.end(), worldPool.end()]).then(() => process.exit(0)));
  });
}
