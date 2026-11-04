import type { ResultSetHeader, RowDataPacket } from 'mysql2/promise';
import type { Db } from '../db/pool.js';

export interface Ban {
  id: number;
  accountId: number;
  reason: string;
  expiresAt: Date | null;
  bannedBy: number | null;
  createdAt: Date;
}

export interface BanRecord extends Ban {
  liftedAt: Date | null;
  liftedBy: number | null;
}

const ACTIVE = 'account_id = ? AND lifted_at IS NULL AND (expires_at IS NULL OR expires_at > ?)';

const toBanRecord = (r: RowDataPacket): BanRecord => ({
  id: r.id,
  accountId: r.account_id,
  reason: r.reason,
  expiresAt: r.expires_at,
  bannedBy: r.banned_by,
  createdAt: r.created_at,
  liftedAt: r.lifted_at,
  liftedBy: r.lifted_by,
});

export async function findActiveBan(db: Db, accountId: number, now: Date): Promise<Ban | undefined> {
  const [rows] = await db.query<RowDataPacket[]>(
    `SELECT id, account_id, reason, expires_at, banned_by, created_at FROM bans WHERE ${ACTIVE}
      ORDER BY created_at DESC, id DESC LIMIT 1`,
    [accountId, now],
  );
  const r = rows[0];
  return (
    r && {
      id: r.id,
      accountId: r.account_id,
      reason: r.reason,
      expiresAt: r.expires_at,
      bannedBy: r.banned_by,
      createdAt: r.created_at,
    }
  );
}

/** Every ban the account ever had, newest first. */
export async function listBans(db: Db, accountId: number): Promise<BanRecord[]> {
  const [rows] = await db.query<RowDataPacket[]>(
    'SELECT * FROM bans WHERE account_id = ? ORDER BY created_at DESC, id DESC',
    [accountId],
  );
  return rows.map(toBanRecord);
}

/** The newest active ban of each listed account that has one. */
export async function findActiveBans(db: Db, accountIds: number[], now: Date): Promise<Map<number, Ban>> {
  const bans = new Map<number, Ban>();
  if (!accountIds.length) return bans;
  const [rows] = await db.query<RowDataPacket[]>(
    `SELECT * FROM bans WHERE account_id IN (?) AND lifted_at IS NULL AND (expires_at IS NULL OR expires_at > ?)
      ORDER BY created_at DESC, id DESC`,
    [accountIds, now],
  );
  for (const r of rows) if (!bans.has(r.account_id)) bans.set(r.account_id, toBanRecord(r));
  return bans;
}

export async function liftActiveBans(db: Db, accountId: number, liftedBy: number, now: Date): Promise<void> {
  await db.query(`UPDATE bans SET lifted_at = ?, lifted_by = ? WHERE ${ACTIVE}`, [now, liftedBy, accountId, now]);
}

export async function insertBan(db: Db, ban: Omit<Ban, 'id'>): Promise<Ban> {
  const [res] = await db.query<ResultSetHeader>(
    'INSERT INTO bans (account_id, reason, expires_at, banned_by, created_at) VALUES (?, ?, ?, ?, ?)',
    [ban.accountId, ban.reason, ban.expiresAt, ban.bannedBy, ban.createdAt],
  );
  return { id: res.insertId, ...ban };
}
