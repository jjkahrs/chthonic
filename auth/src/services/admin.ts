import type { Pool } from 'mysql2/promise';
import type { Clock } from '../clock.js';
import { isDuplicateKey, withTransaction, type Db } from '../db/pool.js';
import { AppError, forbidden } from '../errors.js';
import {
  clearLoginFailures,
  deleteAccount,
  findAccountById,
  insertAccount,
  markEmailVerified,
  searchAccounts,
  setRole,
  type Account,
  type AccountFilter,
  type Role,
} from '../repositories/accounts.js';
import { findActiveBan, findActiveBans, insertBan, liftActiveBans, listBans, type Ban } from '../repositories/bans.js';
import { revokeAllRefreshTokens } from '../repositories/refreshTokens.js';
import { toAccountView, type AccountView, type AuthService } from './auth.js';
import { hashPassword } from './crypto.js';
import type { AuthContext } from './tokens.js';

export interface BanView {
  id: number;
  accountId: number;
  reason: string;
  expiresAt: string | null;
  bannedBy: number | null;
  createdAt: string;
}

export interface BanHistoryView extends BanView {
  liftedAt: string | null;
  liftedBy: number | null;
}

/** What admins and GMs see of an account: the public view plus moderation state. */
export interface AdminAccountView extends AccountView {
  emailVerifiedAt: string | null;
  failedLoginCount: number;
  lockedUntil: string | null;
  activeBan: BanView | null;
}

export interface AccountDetail extends AdminAccountView {
  bans: BanHistoryView[];
}

const toBanView = (ban: Ban): BanView => ({
  id: ban.id,
  accountId: ban.accountId,
  reason: ban.reason,
  expiresAt: ban.expiresAt?.toISOString() ?? null,
  bannedBy: ban.bannedBy,
  createdAt: ban.createdAt.toISOString(),
});

const toAdminAccountView = (a: Account, activeBan: Ban | undefined): AdminAccountView => ({
  ...toAccountView(a),
  emailVerifiedAt: a.emailVerifiedAt?.toISOString() ?? null,
  failedLoginCount: a.failedLoginCount,
  lockedUntil: a.lockedUntil?.toISOString() ?? null,
  activeBan: activeBan ? toBanView(activeBan) : null,
});

async function requireAccount(db: Db, id: number): Promise<Account> {
  const account = await findAccountById(db, id);
  if (!account) throw new AppError(404, 'account_not_found', 'Account not found');
  return account;
}

/** Stops an admin from banning, deleting or demoting themselves and leaving no admin behind. */
function assertNotSelf(adminId: number, accountId: number): void {
  if (adminId === accountId) {
    throw new AppError(400, 'cannot_modify_self', 'Admins cannot ban, delete or change the role of their own account');
  }
}

/** GMs moderate players only; admins can act on any account. */
function assertCanModerate(actor: AuthContext, target: Pick<Account, 'role'>): void {
  if (actor.role !== 'admin' && target.role !== 'player') throw forbidden('GMs can only manage player accounts');
}

export type AdminService = ReturnType<typeof createAdminService>;

export function createAdminService(deps: { pool: Pool; clock: Clock; auth: Pick<AuthService, 'forgotPassword'> }) {
  const { pool, clock, auth } = deps;

  return {
    /** GMs only ever see player accounts. */
    async list(
      actor: AuthContext,
      filter: AccountFilter,
    ): Promise<{ accounts: AdminAccountView[]; total: number; limit: number; offset: number }> {
      if (actor.role !== 'admin') {
        assertCanModerate(actor, { role: filter.role ?? 'player' });
        filter = { ...filter, role: 'player' };
      }
      const now = clock.now();
      const { accounts, total } = await searchAccounts(pool, filter, now);
      const bans = await findActiveBans(pool, accounts.map((a) => a.id), now);
      return {
        accounts: accounts.map((a) => toAdminAccountView(a, bans.get(a.id))),
        total,
        limit: filter.limit,
        offset: filter.offset,
      };
    },

    async get(actor: AuthContext, accountId: number): Promise<AccountDetail> {
      const account = await requireAccount(pool, accountId);
      assertCanModerate(actor, account);
      const bans = await listBans(pool, accountId);
      return {
        ...toAdminAccountView(account, await findActiveBan(pool, accountId, clock.now())),
        bans: bans.map((b) => ({ ...toBanView(b), liftedAt: b.liftedAt?.toISOString() ?? null, liftedBy: b.liftedBy })),
      };
    },

    /** The account starts verified: the admin vouches for the email address. */
    async create(input: { email: string; password: string; role: Role }): Promise<AdminAccountView> {
      const passwordHash = await hashPassword(input.password);
      const now = clock.now();
      let id: number;
      try {
        id = await insertAccount(pool, { email: input.email, passwordHash, role: input.role, emailVerifiedAt: now, createdAt: now });
      } catch (err) {
        if (isDuplicateKey(err)) throw new AppError(409, 'email_taken', 'An account with this email already exists');
        throw err;
      }
      return toAdminAccountView(await requireAccount(pool, id), undefined);
    },

    /** Hard delete. Access JWTs already issued live until they expire. */
    async remove(adminId: number, accountId: number): Promise<void> {
      assertNotSelf(adminId, accountId);
      if (!(await deleteAccount(pool, accountId))) throw new AppError(404, 'account_not_found', 'Account not found');
    },

    async verifyEmail(accountId: number): Promise<void> {
      await requireAccount(pool, accountId);
      await markEmailVerified(pool, accountId, clock.now());
    },

    /** Sends the same email as "forgot password". */
    async sendPasswordReset(accountId: number): Promise<void> {
      await auth.forgotPassword((await requireAccount(pool, accountId)).email);
    },

    async unlock(accountId: number): Promise<void> {
      await requireAccount(pool, accountId);
      await clearLoginFailures(pool, accountId);
    },

    /** Ends every session. Access JWTs already issued live until they expire. */
    async revokeSessions(accountId: number): Promise<void> {
      await requireAccount(pool, accountId);
      await revokeAllRefreshTokens(pool, accountId, clock.now());
    },

    /** Replaces any active ban and ends every session. Access JWTs already issued live until they expire. */
    async ban(actor: AuthContext, accountId: number, input: { reason: string; expiresAt: Date | null }): Promise<BanView> {
      assertNotSelf(actor.accountId, accountId);
      const now = clock.now();
      if (input.expiresAt && input.expiresAt <= now) {
        throw new AppError(400, 'validation_error', 'Request validation failed', [
          { path: 'expiresAt', message: 'expiresAt must be in the future' },
        ]);
      }
      const ban = await withTransaction(pool, async (conn) => {
        assertCanModerate(actor, await requireAccount(conn, accountId));
        await liftActiveBans(conn, accountId, actor.accountId, now);
        await revokeAllRefreshTokens(conn, accountId, now);
        return insertBan(conn, {
          accountId,
          reason: input.reason,
          expiresAt: input.expiresAt,
          bannedBy: actor.accountId,
          createdAt: now,
        });
      });
      return toBanView(ban);
    },

    async unban(actor: AuthContext, accountId: number): Promise<void> {
      assertCanModerate(actor, await requireAccount(pool, accountId));
      await liftActiveBans(pool, accountId, actor.accountId, clock.now());
    },

    /** Takes effect in the next access token the account is issued. */
    async setRole(adminId: number, accountId: number, role: Role): Promise<AccountView> {
      assertNotSelf(adminId, accountId);
      const account = await requireAccount(pool, accountId);
      await setRole(pool, accountId, role);
      return toAccountView({ ...account, role });
    },
  };
}
