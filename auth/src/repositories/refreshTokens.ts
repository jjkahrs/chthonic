import type { RowDataPacket } from 'mysql2/promise';
import type { Db } from '../db/pool.js';

export interface RefreshTokenRow {
  id: number;
  accountId: number;
  familyId: string;
  expiresAt: Date;
  rotatedAt: Date | null;
  revokedAt: Date | null;
}

export async function insertRefreshToken(
  db: Db,
  t: { accountId: number; familyId: string; tokenHash: string; createdAt: Date; expiresAt: Date },
): Promise<void> {
  await db.query(
    'INSERT INTO refresh_tokens (account_id, family_id, token_hash, created_at, expires_at) VALUES (?, ?, ?, ?, ?)',
    [t.accountId, t.familyId, t.tokenHash, t.createdAt, t.expiresAt],
  );
}

/** Row-locks the token so two concurrent refreshes with the same token are serialized. */
export async function findRefreshTokenForUpdate(db: Db, tokenHash: string): Promise<RefreshTokenRow | undefined> {
  const [rows] = await db.query<RowDataPacket[]>(
    'SELECT id, account_id, family_id, expires_at, rotated_at, revoked_at FROM refresh_tokens WHERE token_hash = ? FOR UPDATE',
    [tokenHash],
  );
  const r = rows[0];
  return (
    r && {
      id: r.id,
      accountId: r.account_id,
      familyId: r.family_id,
      expiresAt: r.expires_at,
      rotatedAt: r.rotated_at,
      revokedAt: r.revoked_at,
    }
  );
}

export async function markRefreshTokenRotated(db: Db, id: number, at: Date): Promise<void> {
  await db.query('UPDATE refresh_tokens SET rotated_at = ? WHERE id = ?', [at, id]);
}

export async function revokeRefreshFamily(db: Db, familyId: string, at: Date): Promise<void> {
  await db.query('UPDATE refresh_tokens SET revoked_at = ? WHERE family_id = ? AND revoked_at IS NULL', [at, familyId]);
}

export async function revokeAllRefreshTokens(db: Db, accountId: number, at: Date): Promise<void> {
  await db.query('UPDATE refresh_tokens SET revoked_at = ? WHERE account_id = ? AND revoked_at IS NULL', [
    at,
    accountId,
  ]);
}
