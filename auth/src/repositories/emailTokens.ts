import type { RowDataPacket } from 'mysql2/promise';
import type { Db } from '../db/pool.js';

export type EmailTokenPurpose = 'verify_email' | 'password_reset';

export async function insertEmailToken(
  db: Db,
  t: { accountId: number; purpose: EmailTokenPurpose; tokenHash: string; createdAt: Date; expiresAt: Date },
): Promise<void> {
  await db.query(
    'INSERT INTO email_tokens (account_id, purpose, token_hash, created_at, expires_at) VALUES (?, ?, ?, ?, ?)',
    [t.accountId, t.purpose, t.tokenHash, t.createdAt, t.expiresAt],
  );
}

/** Returns the owning account id if the token exists, is unused and unexpired; row-locks it. */
export async function findUsableEmailToken(
  db: Db,
  tokenHash: string,
  purpose: EmailTokenPurpose,
  now: Date,
): Promise<{ id: number; accountId: number } | undefined> {
  const [rows] = await db.query<RowDataPacket[]>(
    `SELECT id, account_id FROM email_tokens
      WHERE token_hash = ? AND purpose = ? AND used_at IS NULL AND expires_at > ? FOR UPDATE`,
    [tokenHash, purpose, now],
  );
  const r = rows[0];
  return r && { id: r.id, accountId: r.account_id };
}

/** Consumes every outstanding token of this purpose for the account, so older links die too. */
export async function useAllEmailTokens(db: Db, accountId: number, purpose: EmailTokenPurpose, at: Date): Promise<void> {
  await db.query('UPDATE email_tokens SET used_at = ? WHERE account_id = ? AND purpose = ? AND used_at IS NULL', [
    at,
    accountId,
    purpose,
  ]);
}
