import { systemClock } from './clock.js';
import { loadConfig, type Config } from './config.js';
import { migrate } from './db/migrate.js';
import { createPool } from './db/pool.js';
import { createApp } from './http/app.js';
import { loadSigningKeys } from './keys.js';
import { createSmtpMailer } from './mailer.js';
import { seedAdmin } from './services/seed.js';

const log = console;

let config: Config;
try {
  config = loadConfig();
} catch (err) {
  log.error((err as Error).message);
  process.exit(1);
}

await migrate(config);
const pool = createPool(config);
const keys = await loadSigningKeys(config);
await seedAdmin(pool, config, systemClock, log);

const app = createApp({ config, pool, clock: systemClock, mailer: createSmtpMailer(config), keys, log });
const server = app.listen(config.PORT, () => log.info(`chthonic-auth listening on :${config.PORT} (kid ${keys.kid})`));

for (const signal of ['SIGTERM', 'SIGINT'] as const) {
  process.once(signal, () => {
    log.info(`${signal} received, shutting down`);
    server.close(() => void pool.end().then(() => process.exit(0)));
  });
}
