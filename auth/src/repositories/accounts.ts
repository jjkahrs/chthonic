import type { ResultSetHeader, RowDataPacket } from 'mysql2/promise';
import type { Db } from '../db/pool.js';

export const ROLES = ['player', 'gm', 'admin'] as const;
export type Role = (typeof ROLES)[number];

export interface Account {
  id: number;
  email: string;
  passwordHash: string;
  role: Role;
  emailVerifiedAt: Date | null;
  /** Summed over every IP in login_failures (moderation view only; login checks its own IP's row). */
  failedLoginCount: number;
  /** Latest lock over every IP in login_failures (moderation view only). */
  lockedUntil: Date | null;
  createdAt: Date;
}

// accounts.failed_login_count / locked_until are legacy and no longer read: lockout is per
// (account, IP) in login_failures since migration 003.
const SELECT_ACCOUNTS = `SELECT accounts.*,
  (SELECT COALESCE(SUM(f.failed_count), 0) FROM login_failures f WHERE f.account_id = accounts.id) AS lf_failed_count,
  (SELECT MAX(f.locked_until) FROM login_failures f WHERE f.account_id = accounts.id) AS lf_locked_until
  FROM accounts`;

const toAccount = (r: RowDataPacket): Account => ({
  id: r.id,
  email: r.email,
  passwordHash: r.password_hash,
  role: r.role,
  emailVerifiedAt: r.email_verified_at,
  failedLoginCount: Number(r.lf_failed_count),
  lockedUntil: r.lf_locked_until,
  createdAt: r.created_at,
});

export async function findAccountByEmail(db: Db, email: string): Promise<Account | undefined> {
  const [rows] = await db.query<RowDataPacket[]>(`${SELECT_ACCOUNTS} WHERE email = ?`, [email]);
  return rows[0] && toAccount(rows[0]);
}

export async function findAccountById(db: Db, id: number): Promise<Account | undefined> {
  const [rows] = await db.query<RowDataPacket[]>(`${SELECT_ACCOUNTS} WHERE id = ?`, [id]);
  return rows[0] && toAccount(rows[0]);
}

export async function insertAccount(
  db: Db,
  a: Pick<Account, 'email' | 'passwordHash' | 'role' | 'emailVerifiedAt' | 'createdAt'>,
): Promise<number> {
  const [res] = await db.query<ResultSetHeader>(
    'INSERT INTO accounts (email, password_hash, role, email_verified_at, created_at) VALUES (?, ?, ?, ?, ?)',
    [a.email, a.passwordHash, a.role, a.emailVerifiedAt, a.createdAt],
  );
  return res.insertId;
}

/** Replaces the hash only while the email is still unverified; false if it was verified meanwhile. */
export async function setUnverifiedPasswordHash(db: Db, id: number, passwordHash: string): Promise<boolean> {
  const [res] = await db.query<ResultSetHeader>(
    'UPDATE accounts SET password_hash = ? WHERE id = ? AND email_verified_at IS NULL',
    [passwordHash, id],
  );
  return res.affectedRows > 0;
}

export async function markEmailVerified(db: Db, id: number, at: Date): Promise<void> {
  await db.query('UPDATE accounts SET email_verified_at = COALESCE(email_verified_at, ?) WHERE id = ?', [at, id]);
}

export async function setPasswordHash(db: Db, id: number, passwordHash: string): Promise<void> {
  await db.query('UPDATE accounts SET password_hash = ? WHERE id = ?', [passwordHash, id]);
}

export async function setRole(db: Db, id: number, role: Role): Promise<void> {
  await db.query('UPDATE accounts SET role = ? WHERE id = ?', [role, id]);
}

export interface AccountFilter {
  /** Email substring, or an exact id when it is all digits. */
  q?: string;
  role?: Role;
  banned?: boolean;
  verified?: boolean;
  locked?: boolean;
  limit: number;
  offset: number;
}

/** One page of accounts ordered by id, plus the total matching the filter. */
export async function searchAccounts(
  db: Db,
  f: AccountFilter,
  now: Date,
): Promise<{ accounts: Account[]; total: number }> {
  const where: string[] = [];
  const params: unknown[] = [];
  if (f.q) {
    const like = `%${f.q.replace(/[\\%_]/g, '\\$&')}%`;
    if (/^\d+$/.test(f.q)) {
      where.push('(id = ? OR email LIKE ?)');
      params.push(f.q, like);
    } else {
      where.push('email LIKE ?');
      params.push(like);
    }
  }
  if (f.role) {
    where.push('role = ?');
    params.push(f.role);
  }
  if (f.banned !== undefined) {
    where.push(
      `${f.banned ? '' : 'NOT '}EXISTS (SELECT 1 FROM bans b WHERE b.account_id = accounts.id
        AND b.lifted_at IS NULL AND (b.expires_at IS NULL OR b.expires_at > ?))`,
    );
    params.push(now);
  }
  if (f.verified !== undefined) where.push(`email_verified_at IS ${f.verified ? 'NOT ' : ''}NULL`);
  if (f.locked !== undefined) {
    where.push(
      `${f.locked ? '' : 'NOT '}EXISTS (SELECT 1 FROM login_failures f WHERE f.account_id = accounts.id AND f.locked_until > ?)`,
    );
    params.push(now);
  }
  const clause = where.length ? `WHERE ${where.join(' AND ')}` : '';
  const [rows] = await db.query<RowDataPacket[]>(`${SELECT_ACCOUNTS} ${clause} ORDER BY id LIMIT ? OFFSET ?`, [
    ...params,
    f.limit,
    f.offset,
  ]);
  const [count] = await db.query<RowDataPacket[]>(`SELECT COUNT(*) AS total FROM accounts ${clause}`, params);
  return { accounts: rows.map(toAccount), total: Number(count[0]?.total ?? 0) };
}

/** Bans, refresh and email tokens go with it (FK cascade). Returns false when no account has this id. */
export async function deleteAccount(db: Db, id: number): Promise<boolean> {
  const [res] = await db.query<ResultSetHeader>('DELETE FROM accounts WHERE id = ?', [id]);
  return res.affectedRows > 0;
}
