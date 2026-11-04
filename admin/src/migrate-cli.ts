import { loadConfig } from './config.js';
import { migrate } from './db/migrate.js';
import { adminDbOptions } from './db/pool.js';

try {
  const applied = await migrate(adminDbOptions(loadConfig()));
  console.log(applied.length ? `Applied ${applied.length} migration(s)` : 'Database is up to date');
} catch (err) {
  console.error((err as Error).message);
  process.exit(1);
}
