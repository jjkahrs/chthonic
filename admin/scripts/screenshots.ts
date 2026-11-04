/**
 * Screenshots of every admin app page, as admin and as gm, into screenshots/. Not a test suite.
 *
 * Needs the world compose stack (cd ../world && docker compose up -d --build). Headless Chromium only, no window.
 * Signs in with SEED_ADMIN_EMAIL / SEED_ADMIN_PASSWORD from ../world/.env (or the environment), (re)creates a gm
 * account, runs one idle bot so a character is online, and stops then restarts the world container for the
 * realm-unreachable shots.
 *
 *   npx tsx scripts/screenshots.ts            # ADMIN_URL defaults to http://localhost:3200
 */
import { spawn, spawnSync } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { parseEnv } from 'node:util';
import { chromium, type BrowserContext, type Page } from 'playwright';

const worldDir = fileURLToPath(new URL('../../world/', import.meta.url));
const outDir = fileURLToPath(new URL('../screenshots/', import.meta.url));
const worldEnvFile = `${worldDir}.env`;
const env = { ...(existsSync(worldEnvFile) ? parseEnv(readFileSync(worldEnvFile, 'utf8')) : {}), ...process.env };
const BASE = env.ADMIN_URL ?? 'http://localhost:3200';
const REALM_ID = Number(env.REALM_ID ?? 1);
const ADMIN = { email: env.SEED_ADMIN_EMAIL ?? '', password: env.SEED_ADMIN_PASSWORD ?? '' };
const GM = { email: 'gm-screenshots@chthonic.local', password: randomBytes(12).toString('base64url') };
if (!ADMIN.email || !ADMIN.password) throw new Error('Set SEED_ADMIN_EMAIL and SEED_ADMIN_PASSWORD (../world/.env)');

const compose = (...args: string[]) => spawnSync('docker', ['compose', ...args], { cwd: worldDir, stdio: 'ignore' });

async function shot(page: Page, name: string) {
  await page.waitForLoadState('networkidle');
  await page.screenshot({ path: `${outDir}${name}.png`, fullPage: true });
  console.log(`saved ${name}.png`);
}

async function login(context: BrowserContext, who: { email: string; password: string }): Promise<Page> {
  const page = await context.newPage();
  await page.goto(`${BASE}/`);
  await page.getByLabel('Email').fill(who.email);
  await page.getByLabel('Password').fill(who.password);
  await page.getByRole('button', { name: 'Log in' }).click();
  await page.getByRole('link', { name: 'Accounts' }).waitFor();
  return page;
}

/** API calls with the page's session cookies; writes echo the CSRF cookie like the SPA does. */
async function call<T>(page: Page, method: string, path: string, data?: unknown): Promise<T> {
  const csrf = (await page.context().cookies()).find((c) => c.name === 'chthonic_admin_csrf')?.value ?? '';
  const res = await page.request.fetch(`${BASE}/api${path}`, { method, data, headers: { 'X-CSRF-Token': csrf } });
  if (!res.ok()) throw new Error(`${method} ${path}: ${res.status()} ${await res.text()}`);
  return (res.status() === 204 ? undefined : await res.json()) as T;
}

interface Online {
  online: { accountId: number; characterId: number }[];
}

async function waitForOnlineCharacter(page: Page) {
  for (let i = 0; i < 60; i++) {
    const { online } = await call<Online>(page, 'GET', `/realms/${REALM_ID}/online`).catch(() => ({ online: [] }));
    const entry = online.find((o) => o.characterId > 0);
    if (entry) return entry;
    await page.waitForTimeout(1000);
  }
  throw new Error('No bot character came online');
}

mkdirSync(outDir, { recursive: true });
const browser = await chromium.launch({ headless: true });
const viewport = { width: 1440, height: 900 };
const adminCtx = await browser.newContext({ viewport });
const gmCtx = await browser.newContext({ viewport });
// A named container, so the bot can be removed for sure afterwards (killing `docker compose run` leaves it running).
const BOT = 'chthonic-admin-screenshots-bot';
const removeBot = () => spawnSync('docker', ['rm', '-f', BOT], { stdio: 'ignore' });
removeBot();
spawn(
  'docker',
  ['compose', 'run', '--rm', '-T', '--name', BOT, 'bot', '--run-id', `shots${Date.now()}`, '--mode', 'load',
    '--count', '1', '--behaviour', 'idle', '--duration', '300'],
  { cwd: worldDir, stdio: 'ignore' },
);

try {
  // Login page, and a failed login's readable error.
  const anon = await adminCtx.newPage();
  await anon.goto(`${BASE}/`);
  await shot(anon, '01-login');
  await anon.getByLabel('Email').fill(ADMIN.email);
  await anon.getByLabel('Password').fill('wrong-password');
  await anon.getByRole('button', { name: 'Log in' }).click();
  await anon.getByRole('alert').waitFor();
  await shot(anon, '02-login-error');
  await anon.close();

  // Admin first: an admin sign-in refreshes the realm endpoints GMs rely on.
  const admin = await login(adminCtx, ADMIN);
  const existing = await call<{ accounts: { id: number }[] }>(admin, 'GET', `/accounts?q=${GM.email}`);
  for (const a of existing.accounts) await call(admin, 'DELETE', `/accounts/${a.id}`);
  await call(admin, 'POST', '/accounts', { ...GM, role: 'gm' });
  const bots = await waitForOnlineCharacter(admin);
  const charUrl = `${BASE}/characters/${bots.characterId}`;
  const adminId = (await call<{ id: number }>(admin, 'GET', '/session')).id;

  await admin.goto(`${BASE}/accounts`);
  await shot(admin, 'admin-01-accounts');
  await admin.goto(`${BASE}/accounts/new`);
  await shot(admin, 'admin-02-account-create');
  await admin.goto(`${BASE}/accounts/${bots.accountId}`);
  await admin.getByRole('heading', { name: 'Characters' }).waitFor();
  await shot(admin, 'admin-03-account-detail');
  await admin.getByRole('button', { name: 'Ban' }).click();
  await admin.getByRole('dialog').getByLabel('Reason').fill('Botting');
  await shot(admin, 'admin-04-ban-confirm');
  await admin.getByRole('button', { name: 'Cancel' }).click();
  await admin.goto(`${BASE}/characters`);
  await shot(admin, 'admin-05-characters');
  await admin.goto(charUrl);
  await admin.getByText('online', { exact: true }).first().waitFor();
  await shot(admin, 'admin-06-character-detail');
  await admin.getByLabel('Name', { exact: true }).fill('Screenshot');
  await admin.getByRole('button', { name: 'Rename' }).click();
  await admin.getByRole('dialog').waitFor();
  await shot(admin, 'admin-07-character-online-409');
  await admin.getByRole('button', { name: 'Cancel' }).click();
  await admin.goto(`${BASE}/online`);
  await admin.getByRole('cell', { name: String(bots.accountId), exact: true }).waitFor();
  await shot(admin, 'admin-08-online');
  await admin.getByRole('button', { name: 'Kick' }).first().click();
  await shot(admin, 'admin-09-kick-confirm');
  await admin.getByRole('button', { name: 'Cancel' }).click();
  await admin.goto(`${BASE}/realms`);
  await admin.getByRole('cell', { name: 'reachable' }).first().waitFor();
  await shot(admin, 'admin-10-realms');
  await admin.goto(`${BASE}/audit`);
  await shot(admin, 'admin-11-audit');
  await admin.goto(`${BASE}/instances`); // the realm's live map contexts (instance copies once someone is in one)
  await admin.getByRole('cell', { name: 'world' }).first().waitFor();
  await shot(admin, 'admin-14-instances');

  // GM: no Realms/Audit tabs, no create/delete/role/force; bans only on players.
  const gm = await login(gmCtx, GM);
  await gm.goto(`${BASE}/accounts`);
  await shot(gm, 'gm-01-accounts');
  await gm.goto(`${BASE}/accounts/${bots.accountId}`);
  await gm.getByRole('heading', { name: 'Characters' }).waitFor();
  await shot(gm, 'gm-02-account-detail-player');
  await gm.goto(`${BASE}/accounts/${adminId}`); // GMs may only see player accounts: auth answers 403
  await gm.getByRole('alert').waitFor();
  await shot(gm, 'gm-03-account-detail-admin');
  await gm.goto(`${BASE}/characters`);
  await shot(gm, 'gm-04-characters');
  await gm.goto(charUrl);
  await gm.getByText('online', { exact: true }).first().waitFor();
  await shot(gm, 'gm-05-character-detail');
  await gm.goto(`${BASE}/online`);
  await gm.getByRole('cell', { name: String(bots.accountId), exact: true }).waitFor();
  await shot(gm, 'gm-06-online');
  await gm.goto(`${BASE}/realms`); // not a GM route: back to accounts
  await shot(gm, 'gm-07-realms-redirect');
  await gm.goto(`${BASE}/instances`);
  await gm.getByRole('cell', { name: 'world' }).first().waitFor();
  await shot(gm, 'gm-09-instances');

  // Realm unreachable: the world is stopped. Admins get the force checkbox, GMs don't.
  compose('stop', 'world');
  for (const [page, prefix] of [
    [admin, 'admin-12'],
    [gm, 'gm-08'],
  ] as const) {
    await page.goto(charUrl);
    await page.getByRole('button', { name: 'Rename' }).waitFor();
    await page.getByLabel('Name', { exact: true }).fill('Screenshot');
    await page.getByRole('button', { name: 'Rename' }).click();
    await page.locator('.error code', { hasText: 'realm_unreachable' }).nth(1).waitFor();
    await shot(page, `${prefix}-character-unreachable-503`);
  }
  const realmName = (await call<{ realms: { id: number; name: string }[] }>(admin, 'GET', '/realms')).realms.find(
    (r) => r.id === REALM_ID,
  )?.name;
  await admin.goto(`${BASE}/online`);
  await admin.getByRole('button', { name: realmName }).click();
  await admin.locator('.error code', { hasText: 'realm_unreachable' }).waitFor();
  await shot(admin, 'admin-13-online-unreachable');
} finally {
  removeBot();
  compose('start', 'world');
  await browser.close();
}
