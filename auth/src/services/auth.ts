import { randomUUID } from 'node:crypto';
import type { Pool } from 'mysql2/promise';
import type { Clock } from '../clock.js';
import type { Config } from '../config.js';
import { isDuplicateKey, withTransaction, type Db } from '../db/pool.js';
import { AppError, unauthorized } from '../errors.js';
import type { Logger } from '../logger.js';
import type { Mail, Mailer } from '../mailer.js';
import {
  findAccountByEmail,
  findAccountById,
  insertAccount,
  markEmailVerified,
  setPasswordHash,
  setUnverifiedPasswordHash,
  type Account,
} from '../repositories/accounts.js';
import { findActiveBan } from '../repositories/bans.js';
import {
  findUsableEmailToken,
  insertEmailToken,
  useAllEmailTokens,
  type EmailTokenPurpose,
} from '../repositories/emailTokens.js';
import {
  clearLoginFailure,
  clearLoginFailures,
  findLoginFailure,
  recordFailedLogin,
} from '../repositories/loginFailures.js';
import {
  findRefreshTokenForUpdate,
  insertRefreshToken,
  markRefreshTokenRotated,
  revokeAllRefreshTokens,
  revokeRefreshFamily,
} from '../repositories/refreshTokens.js';
import { hashPassword, randomToken, sha256Hex, verifyAgainstDummy, verifyPassword } from './crypto.js';
import type { TokenService } from './tokens.js';

export const MAX_FAILED_LOGINS = 5;
export const LOCKOUT_MS = 15 * 60_000;
const HOUR_MS = 60 * 60_000;
const DAY_MS = 24 * HOUR_MS;

export interface Session {
  tokenType: 'Bearer';
  accessToken: string;
  expiresIn: number;
  accessTokenExpiresAt: string;
  refreshToken: string;
  refreshTokenExpiresAt: string;
}

export interface AccountView {
  id: number;
  email: string;
  role: Account['role'];
  emailVerified: boolean;
  createdAt: string;
}

export const toAccountView = (a: Account): AccountView => ({
  id: a.id,
  email: a.email,
  role: a.role,
  emailVerified: a.emailVerifiedAt !== null,
  createdAt: a.createdAt.toISOString(),
});

const invalidCredentials = () => new AppError(401, 'invalid_credentials', 'Email or password is incorrect');
const invalidRefreshToken = () => new AppError(401, 'invalid_refresh_token', 'Refresh token is invalid or expired');
const invalidEmailToken = () => new AppError(400, 'invalid_token', 'Token is invalid, expired or already used');

export async function assertNotBanned(db: Db, accountId: number, now: Date): Promise<void> {
  const ban = await findActiveBan(db, accountId, now);
  if (ban) {
    throw new AppError(403, 'account_banned', 'This account is banned', {
      reason: ban.reason,
      expiresAt: ban.expiresAt?.toISOString() ?? null,
    });
  }
}

export type AuthService = ReturnType<typeof createAuthService>;

export function createAuthService(deps: {
  pool: Pool;
  clock: Clock;
  mailer: Mailer;
  config: Config;
  tokens: TokenService;
  log: Logger;
}) {
  const { pool, clock, mailer, config, tokens, log } = deps;

  /**
   * Fire-and-forget: the HTTP response must not depend on whether an email was sent,
   * or response timing would reveal which addresses have accounts.
   */
  function sendMail(mail: Mail): void {
    mailer.send(mail).catch((err: Error) => log.error(`Failed to send "${mail.subject}" email: ${err.message}`));
  }

  function link(page: string, token: string): string {
    const base = config.APP_BASE_URL.endsWith('/') ? config.APP_BASE_URL : `${config.APP_BASE_URL}/`;
    const url = new URL(page, base);
    url.searchParams.set('token', token);
    return url.toString();
  }

  async function createEmailToken(db: Db, accountId: number, purpose: EmailTokenPurpose, ttlMs: number) {
    const token = randomToken();
    const now = clock.now();
    await insertEmailToken(db, {
      accountId,
      purpose,
      tokenHash: sha256Hex(token),
      createdAt: now,
      expiresAt: new Date(now.getTime() + ttlMs),
    });
    return token;
  }

  async function sendVerification(db: Db, account: Pick<Account, 'id' | 'email'>): Promise<void> {
    const token = await createEmailToken(db, account.id, 'verify_email', config.EMAIL_VERIFY_TTL_HOURS * HOUR_MS);
    sendMail({
      to: account.email,
      subject: 'Verify your Chthonic account',
      text:
        `Welcome to Chthonic!\n\nVerify your email address by opening this link:\n${link('verify-email', token)}\n\n` +
        `Verification token: ${token}\n\nThis link expires in ${config.EMAIL_VERIFY_TTL_HOURS} hours.`,
    });
  }

  async function issueSession(db: Db, account: Account, familyId: string): Promise<Session> {
    const now = clock.now();
    const refreshToken = randomToken();
    const refreshExpiresAt = new Date(now.getTime() + config.REFRESH_TOKEN_TTL_DAYS * DAY_MS);
    await insertRefreshToken(db, {
      accountId: account.id,
      familyId,
      tokenHash: sha256Hex(refreshToken),
      createdAt: now,
      expiresAt: refreshExpiresAt,
    });
    const access = await tokens.signAccessToken(account);
    return {
      tokenType: 'Bearer',
      accessToken: access.token,
      expiresIn: config.ACCESS_TOKEN_TTL_SECONDS,
      accessTokenExpiresAt: access.expiresAt.toISOString(),
      refreshToken,
      refreshTokenExpiresAt: refreshExpiresAt.toISOString(),
    };
  }

  function sendSignUpNotice(account: Pick<Account, 'email'>): void {
    sendMail({
      to: account.email,
      subject: 'Chthonic account sign-up attempt',
      text:
        'Someone tried to create a Chthonic account with this email address, but you already have one.\n\n' +
        'If this was you, log in or use "forgot password". Otherwise you can ignore this email.',
    });
  }

  /**
   * Email already registered. Verified: the owner gets a notice. Unverified: the latest
   * registrant's password replaces the pending one and every older verify link dies, so
   * whoever registered first can't have the inbox owner verify their password.
   */
  async function registerExisting(account: Account, passwordHash: string): Promise<void> {
    if (account.emailVerifiedAt) return sendSignUpNotice(account);
    const replaced = await withTransaction(pool, async (conn) => {
      if (!(await setUnverifiedPasswordHash(conn, account.id, passwordHash))) return false;
      await useAllEmailTokens(conn, account.id, 'verify_email', clock.now());
      await sendVerification(conn, account);
      return true;
    });
    // Verified between the lookup and the update: treat it as verified.
    if (!replaced) sendSignUpNotice(account);
  }

  return {
    /** Same outcome whether or not the email is taken (see registerExisting for what happens then). */
    async register(email: string, password: string): Promise<void> {
      const passwordHash = await hashPassword(password);
      const existing = await findAccountByEmail(pool, email);
      if (existing) return registerExisting(existing, passwordHash);
      try {
        await withTransaction(pool, async (conn) => {
          const id = await insertAccount(conn, {
            email,
            passwordHash,
            role: 'player',
            emailVerifiedAt: null,
            createdAt: clock.now(),
          });
          await sendVerification(conn, { id, email });
        });
      } catch (err) {
        if (!isDuplicateKey(err)) throw err;
        // Lost a race with a concurrent registration of the same email.
        const winner = await findAccountByEmail(pool, email);
        if (winner) await registerExisting(winner, passwordHash);
      }
    },

    async verifyEmail(token: string): Promise<void> {
      const now = clock.now();
      await withTransaction(pool, async (conn) => {
        const found = await findUsableEmailToken(conn, sha256Hex(token), 'verify_email', now);
        if (!found) throw invalidEmailToken();
        await useAllEmailTokens(conn, found.accountId, 'verify_email', now);
        await markEmailVerified(conn, found.accountId, now);
      });
    },

    async resendVerification(email: string): Promise<void> {
      const account = await findAccountByEmail(pool, email);
      if (account && !account.emailVerifiedAt) await sendVerification(pool, account);
    },

    /**
     * Check order: lockout, password, verified, ban. Unverified/banned status is only
     * revealed to someone who knows the password. Failures and the lockout count per
     * (account, client IP): a lock on one IP never blocks the account from another.
     */
    async login(email: string, password: string, ip: string): Promise<Session> {
      const now = clock.now();
      const account = await findAccountByEmail(pool, email);
      if (!account) {
        await verifyAgainstDummy(password);
        throw invalidCredentials();
      }
      const failure = await findLoginFailure(pool, account.id, ip);
      if (failure?.lockedUntil && failure.lockedUntil > now) {
        throw new AppError(423, 'account_locked', 'Too many failed logins; the account is temporarily locked', {
          lockedUntil: failure.lockedUntil.toISOString(),
        });
      }
      if (!(await verifyPassword(password, account.passwordHash))) {
        await recordFailedLogin(pool, account.id, ip, MAX_FAILED_LOGINS, new Date(now.getTime() + LOCKOUT_MS), now);
        throw invalidCredentials();
      }
      if (failure) await clearLoginFailure(pool, account.id, ip);
      if (!account.emailVerifiedAt) {
        throw new AppError(403, 'email_not_verified', 'Verify your email address before logging in');
      }
      await assertNotBanned(pool, account.id, now);
      return withTransaction(pool, (conn) => issueSession(conn, account, randomUUID().replaceAll('-', '')));
    },

    async refresh(refreshToken: string): Promise<Session> {
      const now = clock.now();
      const outcome = await withTransaction(pool, async (conn) => {
        const row = await findRefreshTokenForUpdate(conn, sha256Hex(refreshToken));
        if (!row) throw invalidRefreshToken();
        // Ban first: banning revoked the tokens, but the player should still learn why.
        await assertNotBanned(conn, row.accountId, now);
        if (row.revokedAt || row.expiresAt <= now) throw invalidRefreshToken();
        if (row.rotatedAt) {
          // A rotated token came back: it was stolen or replayed. Kill the whole session.
          // Returned rather than thrown so the revoke commits.
          await revokeRefreshFamily(conn, row.familyId, now);
          return { reused: true as const, accountId: row.accountId };
        }
        const account = await findAccountById(conn, row.accountId);
        if (!account) throw invalidRefreshToken();
        await markRefreshTokenRotated(conn, row.id, now);
        return { reused: false as const, session: await issueSession(conn, account, row.familyId) };
      });
      if (outcome.reused) {
        log.warn(`Refresh token reuse detected for account ${outcome.accountId}; session revoked`);
        throw invalidRefreshToken();
      }
      return outcome.session;
    },

    /** Ends the session the refresh token belongs to. Unknown tokens are ignored. */
    async logout(refreshToken: string): Promise<void> {
      await withTransaction(pool, async (conn) => {
        const row = await findRefreshTokenForUpdate(conn, sha256Hex(refreshToken));
        if (row) await revokeRefreshFamily(conn, row.familyId, clock.now());
      });
    },

    async forgotPassword(email: string): Promise<void> {
      const account = await findAccountByEmail(pool, email);
      if (!account) return;
      const ttlMinutes = config.PASSWORD_RESET_TTL_MINUTES;
      const token = await createEmailToken(pool, account.id, 'password_reset', ttlMinutes * 60_000);
      sendMail({
        to: account.email,
        subject: 'Reset your Chthonic password',
        text:
          `Reset your password by opening this link:\n${link('reset-password', token)}\n\n` +
          `Reset token: ${token}\n\nThis link expires in ${ttlMinutes} minutes. ` +
          'If you did not ask for this, ignore this email.',
      });
    },

    /** Also unlocks the account from every IP and marks the email verified: the user just proved they own the inbox. */
    async resetPassword(token: string, newPassword: string): Promise<void> {
      const passwordHash = await hashPassword(newPassword);
      const now = clock.now();
      await withTransaction(pool, async (conn) => {
        const found = await findUsableEmailToken(conn, sha256Hex(token), 'password_reset', now);
        if (!found) throw invalidEmailToken();
        await useAllEmailTokens(conn, found.accountId, 'password_reset', now);
        await setPasswordHash(conn, found.accountId, passwordHash);
        await clearLoginFailures(conn, found.accountId);
        await markEmailVerified(conn, found.accountId, now);
        await revokeAllRefreshTokens(conn, found.accountId, now);
      });
    },

    async me(accountId: number): Promise<AccountView> {
      const account = await findAccountById(pool, accountId);
      if (!account) throw unauthorized();
      return toAccountView(account);
    },
  };
}
