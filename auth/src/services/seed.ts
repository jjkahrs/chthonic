import type { Clock } from '../clock.js';
import type { Config } from '../config.js';
import { isDuplicateKey, type Db } from '../db/pool.js';
import type { Logger } from '../logger.js';
import { findAccountByEmail, insertAccount } from '../repositories/accounts.js';
import { hashPassword } from './crypto.js';

/**
 * Creates the first admin, pre-verified, if SEED_ADMIN_EMAIL is set and no account uses it yet.
 * An existing account with that email is left untouched, so restarts never duplicate or reset it.
 */
export async function seedAdmin(db: Db, config: Config, clock: Clock, log: Logger): Promise<void> {
  if (!config.SEED_ADMIN_EMAIL || !config.SEED_ADMIN_PASSWORD) return;
  const email = config.SEED_ADMIN_EMAIL.trim().toLowerCase();
  if (await findAccountByEmail(db, email)) return;
  const now = clock.now();
  try {
    await insertAccount(db, {
      email,
      passwordHash: await hashPassword(config.SEED_ADMIN_PASSWORD),
      role: 'admin',
      emailVerifiedAt: now,
      createdAt: now,
    });
    log.info(`Seeded admin account ${email}`);
  } catch (err) {
    // Another instance seeded it first.
    if (!isDuplicateKey(err)) throw err;
  }
}
