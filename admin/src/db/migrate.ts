import { readdir, readFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import mysql, { type PoolOptions, type RowDataPacket } from 'mysql2/promise';

// Resolves to admin/migrations from both src/db and dist/db.
const DEFAULT_DIR = fileURLToPath(new URL('../../migrations', import.meta.url));
const LOCK_NAME = 'chthonic_admin_migrate';

/**
 * Applies migrations/*.sql to chthonic_admin in filename order, once each, tracked in schema_migrations.
 * A named lock stops two instances from migrating at the same time.
 * MySQL DDL auto-commits, so a file that fails halfway must be fixed by hand.
 */
export async function migrate(
  options: PoolOptions,
  dir = DEFAULT_DIR,
  log: (msg: string) => void = console.log,
): Promise<string[]> {
  const conn = await mysql.createConnection({ ...options, multipleStatements: true });
  try {
    const [lock] = await conn.query<RowDataPacket[]>('SELECT GET_LOCK(?, 60) AS ok', [LOCK_NAME]);
    if (lock[0]?.ok !== 1) throw new Error('Timed out waiting for the migration lock');
    try {
      await conn.query(
        'CREATE TABLE IF NOT EXISTS schema_migrations (name VARCHAR(255) NOT NULL PRIMARY KEY, applied_at DATETIME(3) NOT NULL)',
      );
      const [rows] = await conn.query<RowDataPacket[]>('SELECT name FROM schema_migrations');
      const applied = new Set(rows.map((r) => r.name as string));
      const pending = (await readdir(dir)).filter((f) => f.endsWith('.sql') && !applied.has(f)).sort();
      for (const file of pending) {
        await conn.query(await readFile(path.join(dir, file), 'utf8'));
        await conn.query('INSERT INTO schema_migrations (name, applied_at) VALUES (?, ?)', [file, new Date()]);
        log(`Applied migration ${file}`);
      }
      return pending;
    } finally {
      await conn.query('SELECT RELEASE_LOCK(?)', [LOCK_NAME]);
    }
  } finally {
    await conn.end();
  }
}
