import { describe, expect, it } from 'vitest';
import { ADMIN, GM, useTestApp, expectError, type Client } from './helpers.js';

const ctx = useTestApp();

type Method = 'get' | 'post' | 'put' | 'delete';
type Route = [Method, string];

// :id placeholders are replaced with an existing player / character / realm, so a 403 means the role gate.
const STAFF: Route[] = [
  ['get', '/api/session'],
  ['get', '/api/accounts'],
  ['get', '/api/accounts/:player'],
  ['post', '/api/accounts/:player/ban'],
  ['delete', '/api/accounts/:player/ban'],
  ['get', '/api/characters'],
  ['get', '/api/characters/:char'],
  ['put', '/api/characters/:char/name'],
  ['put', '/api/characters/:char/position'],
  ['put', '/api/characters/:char/components'],
  ['put', '/api/characters/:char/options'],
  ['delete', '/api/characters/:char'],
  ['post', '/api/characters/:char/restore'],
  ['get', '/api/realms'],
  ['get', '/api/realms/:realm/online'],
  ['post', '/api/realms/:realm/kick'],
];

const ADMIN_ONLY: Route[] = [
  ['post', '/api/accounts'],
  ['delete', '/api/accounts/:player'],
  ['put', '/api/accounts/:player/role'],
  ['post', '/api/accounts/:player/verify-email'],
  ['post', '/api/accounts/:player/password-reset'],
  ['post', '/api/accounts/:player/unlock'],
  ['post', '/api/accounts/:player/revoke-sessions'],
  ['get', '/api/realms/:realm'],
  ['post', '/api/realms'],
  ['put', '/api/realms/:realm'],
  ['delete', '/api/realms/:realm'],
  ['get', '/api/audit'],
];

let char = 0;
const resolve = (path: string) =>
  path
    .replace(':player', String(ctx.ids.player))
    .replace(':char', String(char))
    .replace(':realm', String(ctx.realmId));
const call = (client: Client, [method, path]: Route) => client[method](resolve(path));

describe('permissions', () => {
  it.each([...STAFF, ...ADMIN_ONLY])('%s %s → 401 anonymous (with a valid CSRF token)', async (method, path) => {
    const agent = ctx.api();
    const first = await agent.get('/api/session');
    const csrf = /chthonic_admin_csrf=([^;]+)/.exec([first.headers['set-cookie']].flat().join(';'))![1]!;
    expectError(await agent[method](resolve(path)).set('X-CSRF-Token', csrf), 401, 'unauthorized');
  });

  it.each(ADMIN_ONLY)('%s %s → 403 for gm, audited when mutating', async (method, path) => {
    const gm = await ctx.login(GM);
    expectError(await call(gm, [method, path]), 403, 'forbidden');
    const rows = await ctx.auditRows();
    expect(rows.map((r) => [r.actor_role, r.result])).toEqual(method === 'get' ? [] : [['gm', 'forbidden']]);
  });

  it.each(STAFF)('%s %s → allowed for gm and admin (not 401/403)', async (method, path) => {
    char = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    for (const email of [GM, ADMIN]) {
      const res = await call(await ctx.login(email), [method, path]);
      expect([401, 403], `${email}: ${JSON.stringify(res.body)}`).not.toContain(res.status);
    }
  });

  it.each(ADMIN_ONLY)('%s %s → allowed for admin', async (method, path) => {
    const res = await call(await ctx.login(ADMIN), [method, path]);
    expect([401, 403], JSON.stringify(res.body)).not.toContain(res.status);
  });
});
