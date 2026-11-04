import { generateKeyPairSync } from 'node:crypto';
import { readFileSync } from 'node:fs';
import http from 'node:http';
import type { AddressInfo } from 'node:net';
import { fileURLToPath } from 'node:url';
import { parseEnv } from 'node:util';
import mysql, { type Pool, type RowDataPacket } from 'mysql2/promise';
import supertest from 'supertest';
import { afterAll, beforeAll, beforeEach, expect } from 'vitest';
// The real auth service, in-process on its own test database.
import { loadConfig as loadAuthConfig } from '../../auth/src/config.js';
import { migrate as migrateAuth } from '../../auth/src/db/migrate.js';
import { createPool as createAuthPool } from '../../auth/src/db/pool.js';
import { createApp as createAuthApp } from '../../auth/src/http/app.js';
import { signingKeysFromPem } from '../../auth/src/keys.js';
import { insertAccount } from '../../auth/src/repositories/accounts.js';
import { hashPassword } from '../../auth/src/services/crypto.js';
import { createAuthClient } from '../src/authClient.js';
import type { Clock } from '../src/clock.js';
import { loadConfig, type Config } from '../src/config.js';
import { migrate } from '../src/db/migrate.js';
import { adminDbOptions, createPool, worldDbOptions } from '../src/db/pool.js';
import { createApp, type AppDeps } from '../src/http/app.js';
import { COOKIES, CSRF_HEADER } from '../src/http/middleware.js';
import { silentLogger } from '../src/logger.js';
import type { Role } from '../src/services/session.js';
import { createWorldAdminClient } from '../src/worldAdmin.js';
import { CERT, FakeWorld } from './fakeWorld.js';

export const PASSWORD = 'correct horse battery';
export const WORLD_SECRET = 'test-world-admin-secret';
export const MINUTE = 60_000;

export class FakeClock implements Clock {
  private current = new Date('2030-01-01T00:00:00.000Z');
  now(): Date {
    return new Date(this.current);
  }
  advance(ms: number): void {
    this.current = new Date(this.current.getTime() + ms);
  }
}

export function testConfig(overrides: Record<string, string> = {}): Config {
  return loadConfig({
    ...process.env,
    ADMIN_DB_NAME: process.env.TEST_ADMIN_DB_NAME ?? 'chthonic_admin_test',
    WORLD_DB_NAME: process.env.TEST_WORLD_DB_NAME ?? 'chthonic_world_test',
    AUTH_URL: 'http://127.0.0.1:1', // replaced by the in-process auth's address
    JWT_ISSUER: 'chthonic-auth',
    JWT_AUDIENCE: 'chthonic',
    COOKIE_SECURE: 'false',
    WORLD_ADMIN_SECRET: WORLD_SECRET,
    WORLD_ADMIN_TIMEOUT_MS: '1500',
    ...overrides,
  });
}

const rootOptions = (config: Config) => ({
  host: config.DB_HOST,
  port: config.DB_PORT,
  user: 'root',
  password: process.env.TEST_MYSQL_ROOT_PASSWORD,
  timezone: 'Z' as const,
});

/**
 * Creates the test databases and the two least-privilege users exactly as production expects them:
 * ALL on the admin DB, SELECT + UPDATE on the world DB's characters table. The world schema comes from
 * world/migrations. Idempotent.
 */
async function provision(config: Config): Promise<void> {
  const root = await mysql.createConnection(rootOptions(config));
  try {
    for (const [db, user, password] of [
      [config.ADMIN_DB_NAME, config.ADMIN_DB_USER, config.ADMIN_DB_PASSWORD],
      [config.WORLD_DB_NAME, config.WORLD_DB_USER, config.WORLD_DB_PASSWORD],
    ] as const) {
      await root.query(`CREATE DATABASE IF NOT EXISTS \`${db}\``);
      await root.query('CREATE USER IF NOT EXISTS ?@? IDENTIFIED BY ?', [user, '%', password]);
      await root.query('ALTER USER ?@? IDENTIFIED BY ?', [user, '%', password]);
    }
    await root.query(`GRANT ALL PRIVILEGES ON \`${config.ADMIN_DB_NAME}\`.* TO ?@?`, [config.ADMIN_DB_USER, '%']);
  } finally {
    await root.end();
  }
  const worldMigrations = fileURLToPath(new URL('../../world/migrations', import.meta.url));
  await migrate({ ...rootOptions(config), database: config.WORLD_DB_NAME }, worldMigrations, () => {});
  const root2 = await mysql.createConnection(rootOptions(config));
  try {
    await root2.query(`GRANT SELECT, UPDATE ON \`${config.WORLD_DB_NAME}\`.characters TO ?@?`, [
      config.WORLD_DB_USER,
      '%',
    ]);
  } finally {
    await root2.end();
  }
  await migrate(adminDbOptions(config), undefined, () => {});
}

function authConfig() {
  const env = parseEnv(readFileSync(new URL('../../auth/.env', import.meta.url), 'utf8'));
  return loadAuthConfig({
    ...env,
    DB_NAME: env.TEST_DB_NAME ?? 'chthonic_auth_test',
    APP_BASE_URL: 'http://chthonic.test/account',
    SEED_ADMIN_EMAIL: '',
    SEED_ADMIN_PASSWORD: '',
    JWT_PRIVATE_KEY: '',
    JWT_PRIVATE_KEY_FILE: '',
  });
}

const AUTH_TABLES = ['rate_limits', 'refresh_tokens', 'email_tokens', 'bans', 'realms', 'accounts'];

/** A signed-in browser: keeps cookies and sends the CSRF header on every non-GET. */
export interface Client {
  agent: supertest.Agent;
  csrf: string;
  get(path: string): supertest.Test;
  post(path: string, body?: object): supertest.Test;
  put(path: string, body?: object): supertest.Test;
  delete(path: string, body?: object): supertest.Test;
}

export interface TestContext {
  config: Config;
  clock: FakeClock;
  world: FakeWorld;
  /** Root on the shared MySQL: fixtures and assertions only; the app uses the restricted users. */
  root: Pool;
  authPool: Pool;
  app: http.RequestListener;
  /** What app was built from, to build variants. */
  deps: AppDeps;
  /** Account ids of the accounts every test starts with. */
  ids: { admin: number; gm: number; player: number; otherPlayer: number };
  realmId: number;
  api(): supertest.Agent;
  createAccount(email: string, role?: Role): Promise<number>;
  login(email: string): Promise<Client>;
  addRealm(name: string, adminPort: number | null): Promise<number>;
  addCharacter(c: { accountId: number; name: string; realmId?: number; deleted?: boolean }): Promise<number>;
  character(id: number): Promise<RowDataPacket>;
  auditRows(): Promise<RowDataPacket[]>;
}

export const ADMIN = 'admin@example.com';
export const GM = 'gm@example.com';
export const PLAYER = 'player@example.com';
export const OTHER = 'other@example.com';

export function cookieFrom(res: supertest.Response, name: string): string | undefined {
  const header = [res.headers['set-cookie'] ?? []].flat() as string[];
  const raw = header.find((c) => c.startsWith(`${name}=`));
  return raw?.slice(name.length + 1).split(';')[0];
}

/**
 * One admin app per test file on real MySQL with the real auth app and a fake world admin port; a shared fake clock.
 * Before each test every table is emptied and admin, gm and two player accounts plus one realm (on the fake world)
 * are created; the admin signs in once so GM requests can see the realm endpoint.
 */
export function useTestApp(): TestContext {
  const config = testConfig();
  const ctx = { config, world: new FakeWorld(WORLD_SECRET) } as TestContext;
  let authServer: http.Server;
  let authPool: Pool;
  let adminPool: Pool;
  let worldPool: Pool;
  let passwordHash: string;

  beforeAll(async () => {
    await provision(config);
    ctx.root = mysql.createPool({ ...rootOptions(config), connectionLimit: 2 });
    adminPool = createPool(adminDbOptions(config));
    worldPool = createPool(worldDbOptions(config));
    const authCfg = authConfig();
    await migrateAuth(authCfg, undefined, () => {});
    authPool = ctx.authPool = createAuthPool(authCfg);
    const { privateKey } = generateKeyPairSync('rsa', { modulusLength: 2048 });
    const keys = await signingKeysFromPem(privateKey.export({ format: 'pem', type: 'pkcs8' }) as string);
    // Auth and admin share the clock, so token expiry can be tested by moving it.
    ctx.clock = new FakeClock();
    const clock: Clock = { now: () => ctx.clock.now() };
    const authApp = createAuthApp({
      config: authCfg,
      pool: authPool,
      clock,
      mailer: { send: async () => {} },
      keys,
      log: silentLogger,
    });
    authServer = http.createServer(authApp);
    await new Promise<void>((resolve) => authServer.listen(0, '127.0.0.1', resolve));
    const authUrl = `http://127.0.0.1:${(authServer.address() as AddressInfo).port}`;
    await ctx.world.start();
    passwordHash = await hashPassword(PASSWORD);

    ctx.deps = {
      config,
      adminPool,
      worldPool,
      clock,
      auth: createAuthClient(authUrl),
      world: createWorldAdminClient({ secret: WORLD_SECRET, ca: CERT, timeoutMs: config.WORLD_ADMIN_TIMEOUT_MS }),
      log: silentLogger,
    };
    ctx.app = createApp(ctx.deps);
  });

  beforeEach(async () => {
    ctx.clock = new FakeClock();
    ctx.world.reset();
    await authPool.query('SET FOREIGN_KEY_CHECKS = 0');
    for (const t of AUTH_TABLES) await authPool.query(`TRUNCATE TABLE ${t}`);
    await authPool.query('SET FOREIGN_KEY_CHECKS = 1');
    await ctx.root.query(`TRUNCATE TABLE \`${config.ADMIN_DB_NAME}\`.admin_audit`);
    await ctx.root.query(`TRUNCATE TABLE \`${config.ADMIN_DB_NAME}\`.realm_endpoints`);
    await ctx.root.query(`TRUNCATE TABLE \`${config.WORLD_DB_NAME}\`.characters`);

    ctx.ids = {
      admin: await ctx.createAccount(ADMIN, 'admin'),
      gm: await ctx.createAccount(GM, 'gm'),
      player: await ctx.createAccount(PLAYER),
      otherPlayer: await ctx.createAccount(OTHER),
    };
    ctx.realmId = await ctx.addRealm('Alpha', ctx.world.port);
    await ctx.login(ADMIN); // syncs the realm endpoint copy
    await ctx.root.query(`TRUNCATE TABLE \`${config.ADMIN_DB_NAME}\`.admin_audit`);
  });

  afterAll(async () => {
    await new Promise((resolve) => authServer?.close(resolve));
    await ctx.world.stop();
    await Promise.all([ctx.root?.end(), adminPool?.end(), worldPool?.end(), authPool?.end()]);
  });

  ctx.api = () => supertest.agent(ctx.app);

  ctx.createAccount = async (email, role = 'player') => {
    const now = ctx.clock.now();
    return insertAccount(authPool, { email, passwordHash, role, emailVerifiedAt: now, createdAt: now });
  };

  ctx.login = async (email) => {
    const agent = ctx.api();
    const first = await agent.get('/api/session');
    const res = await agent
      .post('/api/session/login')
      .set(CSRF_HEADER, cookieFrom(first, COOKIES.csrf) ?? '')
      .send({ email, password: PASSWORD });
    expect(res.status, JSON.stringify(res.body)).toBe(200);
    const csrf = cookieFrom(res, COOKIES.csrf)!;
    const send = (method: 'post' | 'put' | 'delete') => (path: string, body?: object) => {
      const req = agent[method](path).set(CSRF_HEADER, csrf);
      return body === undefined ? req : req.send(body);
    };
    return { agent, csrf, get: (path) => agent.get(path), post: send('post'), put: send('put'), delete: send('delete') };
  };

  ctx.addRealm = async (name, adminPort) => {
    const now = ctx.clock.now();
    const [res] = await authPool.query<mysql.ResultSetHeader>(
      `INSERT INTO realms (name, host, port, admin_host, admin_port, region, status, sort_order, created_at, updated_at)
       VALUES (?, '127.0.0.1', 7777, ?, ?, 'local', 'online', 0, ?, ?)`,
      [name, adminPort === null ? null : '127.0.0.1', adminPort, now, now],
    );
    return res.insertId;
  };

  ctx.addCharacter = async ({ accountId, name, realmId = ctx.realmId, deleted = false }) => {
    const now = ctx.clock.now();
    const [res] = await ctx.root.query<mysql.ResultSetHeader>(
      `INSERT INTO \`${config.WORLD_DB_NAME}\`.characters
        (realm_id, account_id, name, pos_x, pos_y, pos_z, heading, created_at, deleted_at, components, options)
       VALUES (?, ?, ?, 1, 2, 3, 0.5, ?, ?, '{"Hp": {"hp": 10}}', '{"class": "warrior"}')`,
      [realmId, accountId, name, now, deleted ? now : null],
    );
    return res.insertId;
  };

  ctx.character = async (id) => {
    const [rows] = await ctx.root.query<RowDataPacket[]>(
      `SELECT * FROM \`${config.WORLD_DB_NAME}\`.characters WHERE id = ?`,
      [id],
    );
    return rows[0]!;
  };

  ctx.auditRows = async () => {
    const [rows] = await ctx.root.query<RowDataPacket[]>(
      `SELECT * FROM \`${config.ADMIN_DB_NAME}\`.admin_audit ORDER BY id`,
    );
    return rows;
  };

  return ctx;
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
  return res.body.error;
}

/** Puts an account online on the fake world, in the world as charId (0 = lobby). */
export function goOnline(ctx: TestContext, accountId: number, charId = 0, name = ''): void {
  ctx.world.sessions.push({ accountId, charId, name, map: 1, sessionId: ctx.world.sessions.length + 1, connectedAtMs: 1_900_000_000_000 });
}
