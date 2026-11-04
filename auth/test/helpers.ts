import { generateKeyPairSync } from 'node:crypto';
import type { Pool } from 'mysql2/promise';
import supertest from 'supertest';
import { afterAll, beforeEach, expect } from 'vitest';
import type { Clock } from '../src/clock.js';
import { loadConfig, type Config } from '../src/config.js';
import { migrate } from '../src/db/migrate.js';
import { createPool } from '../src/db/pool.js';
import { createApp, type AppDeps } from '../src/http/app.js';
import { signingKeysFromPem, type SigningKeys } from '../src/keys.js';
import { silentLogger } from '../src/logger.js';
import type { Mail, Mailer } from '../src/mailer.js';
import { insertAccount, type Role } from '../src/repositories/accounts.js';
import { hashPassword } from '../src/services/crypto.js';
import type { Session } from '../src/services/auth.js';

export const MINUTE = 60_000;
export const HOUR = 60 * MINUTE;
export const DAY = 24 * HOUR;
export const PASSWORD = 'correct horse battery';

export class FakeClock implements Clock {
  private current = new Date('2030-01-01T00:00:00.000Z');
  now(): Date {
    return new Date(this.current);
  }
  advance(ms: number): void {
    this.current = new Date(this.current.getTime() + ms);
  }
}

export class StubMailer implements Mailer {
  sent: Mail[] = [];
  async send(mail: Mail): Promise<void> {
    this.sent.push(mail);
  }
  to(email: string): Mail[] {
    return this.sent.filter((m) => m.to === email);
  }
  /** Token from the link in the latest email to this address. */
  tokenFor(email: string): string {
    const mails = this.to(email);
    const match = mails.at(-1)?.text.match(/token=([\w-]+)/);
    if (!match?.[1]) throw new Error(`No token email for ${email}`);
    return match[1];
  }
}

export function testConfig(overrides: Record<string, string> = {}): Config {
  return loadConfig({
    ...process.env,
    DB_NAME: process.env.TEST_DB_NAME ?? 'chthonic_auth_test',
    APP_BASE_URL: 'http://chthonic.test/account',
    SEED_ADMIN_EMAIL: '',
    SEED_ADMIN_PASSWORD: '',
    JWT_PRIVATE_KEY: '',
    JWT_PRIVATE_KEY_FILE: '',
    ...overrides,
  });
}

let testKeys: Promise<SigningKeys> | undefined;
export function generateKeys(): Promise<SigningKeys> {
  const { privateKey } = generateKeyPairSync('rsa', { modulusLength: 2048 });
  return signingKeysFromPem(privateKey.export({ format: 'pem', type: 'pkcs8' }) as string);
}

const TABLES = ['rate_limits', 'refresh_tokens', 'email_tokens', 'bans', 'realms', 'accounts'];

export async function resetDb(pool: Pool): Promise<void> {
  const conn = await pool.getConnection();
  try {
    await conn.query('SET FOREIGN_KEY_CHECKS = 0');
    for (const table of TABLES) await conn.query(`TRUNCATE TABLE ${table}`);
    await conn.query('SET FOREIGN_KEY_CHECKS = 1');
  } finally {
    conn.release();
  }
}

export interface TestContext {
  config: Config;
  pool: Pool;
  clock: FakeClock;
  mailer: StubMailer;
  keys: SigningKeys;
  api: () => supertest.Agent;
  /** A fresh app on the same DB, as after a process restart; deps can be swapped. */
  restart: (overrides?: Partial<AppDeps>) => void;
}

/** One app per test file, real MySQL, fake clock and mailer; tables are emptied before each test. */
export function useTestApp(): TestContext {
  const config = testConfig();
  const pool = createPool(config);
  const ctx = { config, pool } as TestContext;

  const build = async () => {
    ctx.clock = new FakeClock();
    ctx.mailer = new StubMailer();
    ctx.keys = await (testKeys ??= generateKeys());
    ctx.restart = (overrides = {}) => {
      const app = createApp({ ...ctx, log: silentLogger, ...overrides });
      ctx.api = () => supertest.agent(app);
    };
    ctx.restart();
  };

  beforeEach(async () => {
    await migrate(config, undefined, () => {});
    await resetDb(pool);
    await build();
  });
  afterAll(() => pool.end());
  return ctx;
}

export async function createAccount(
  ctx: TestContext,
  email: string,
  opts: { role?: Role; verified?: boolean; password?: string } = {},
): Promise<number> {
  const now = ctx.clock.now();
  return insertAccount(ctx.pool, {
    email,
    passwordHash: await hashPassword(opts.password ?? PASSWORD),
    role: opts.role ?? 'player',
    emailVerifiedAt: opts.verified === false ? null : now,
    createdAt: now,
  });
}

export async function login(ctx: TestContext, email: string, password = PASSWORD): Promise<Session> {
  const res = await ctx.api().post('/v1/auth/login').send({ email, password });
  expect(res.status, JSON.stringify(res.body)).toBe(200);
  return res.body as Session;
}

/** Asserts the standard {error:{code,message,details?}} shape and returns the error. */
export function expectError(
  res: supertest.Response,
  status: number,
  code: string,
): { code: string; message: string; details?: unknown } {
  expect(res.status, JSON.stringify(res.body)).toBe(status);
  expect(res.headers['content-type']).toMatch(/application\/json/);
  expect(Object.keys(res.body)).toEqual(['error']);
  expect(res.body.error.code).toBe(code);
  expect(typeof res.body.error.message).toBe('string');
  const allowed = ['code', 'message', 'details'];
  expect(Object.keys(res.body.error).every((k) => allowed.includes(k))).toBe(true);
  return res.body.error;
}

export const bearer = (token: string) => ({ Authorization: `Bearer ${token}` });
