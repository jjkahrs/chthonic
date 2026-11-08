import type { RowDataPacket } from 'mysql2/promise';
import type { Db } from '../db/pool.js';

/** Rows untouched this long are dropped once they hold no active lock. */
const STALE_MS = 24 * 60 * 60_000;

export interface LoginFailure {
  failedCount: number;
  lockedUntil: Date | null;
}

export async function findLoginFailure(db: Db, accountId: number, ip: string): Promise<LoginFailure | undefined> {
  const [rows] = await db.query<RowDataPacket[]>(
    'SELECT failed_count, locked_until FROM login_failures WHERE account_id = ? AND ip = ?',
    [accountId, ip],
  );
  const r = rows[0];
  return r && { failedCount: r.failed_count, lockedUntil: r.locked_until };
}

/**
 * Counts a failed login from this IP; once maxFailures is reached that (account, IP) is
 * locked and its counter restarts. Prunes stale rows first (bounded, like rate_limits).
 */
export async function recordFailedLogin(
  db: Db,
  accountId: number,
  ip: string,
  maxFailures: number,
  lockUntil: Date,
  now: Date,
): Promise<void> {
  await db.query(
    `DELETE FROM login_failures WHERE (locked_until IS NULL OR locked_until <= ?)
       AND ((failed_count = 0 AND locked_until IS NOT NULL) OR updated_at <= ?) LIMIT 1000`,
    [now, new Date(now.getTime() - STALE_MS)],
  );
  await db.query(
    `INSERT INTO login_failures (account_id, ip, failed_count, updated_at) VALUES (?, ?, 1, ?)
     ON DUPLICATE KEY UPDATE failed_count = failed_count + 1, updated_at = ?`,
    [accountId, ip, now, now],
  );
  await db.query(
    'UPDATE login_failures SET locked_until = ?, failed_count = 0 WHERE account_id = ? AND ip = ? AND failed_count >= ?',
    [lockUntil, accountId, ip, maxFailures],
  );
}

export async function clearLoginFailure(db: Db, accountId: number, ip: string): Promise<void> {
  await db.query('DELETE FROM login_failures WHERE account_id = ? AND ip = ?', [accountId, ip]);
}

/** Every IP's counter and lock for the account (password reset, admin unlock). */
export async function clearLoginFailures(db: Db, accountId: number): Promise<void> {
  await db.query('DELETE FROM login_failures WHERE account_id = ?', [accountId]);
}
