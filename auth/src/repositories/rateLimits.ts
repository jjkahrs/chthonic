import type { RowDataPacket } from 'mysql2/promise';
import type { Db } from '../db/pool.js';

/** Adds one hit to the (bucket, ip, window) counter and returns the new total. */
export async function addHit(db: Db, bucket: string, ip: string, windowStart: Date, expiresAt: Date): Promise<number> {
  await db.query(
    `INSERT INTO rate_limits (bucket, ip, window_start, hits, expires_at) VALUES (?, ?, ?, 1, ?)
     ON DUPLICATE KEY UPDATE hits = hits + 1`,
    [bucket, ip, windowStart, expiresAt],
  );
  const [rows] = await db.query<RowDataPacket[]>(
    'SELECT hits FROM rate_limits WHERE bucket = ? AND ip = ? AND window_start = ?',
    [bucket, ip, windowStart],
  );
  return rows[0]?.hits ?? 1;
}

/** Bounded delete so pruning never turns into one long-running statement. */
export async function pruneExpiredHits(db: Db, now: Date): Promise<void> {
  await db.query('DELETE FROM rate_limits WHERE expires_at <= ? LIMIT 1000', [now]);
}
