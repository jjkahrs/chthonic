import { beforeEach, describe, expect, it } from 'vitest';
import { DAY, HOUR, bearer, createAccount, expectError, login, useTestApp } from './helpers.js';

const ctx = useTestApp();
const PLAYER = 'player@example.com';
const ADMIN = 'admin@example.com';

let adminId: number;
let playerId: number;
let adminToken: string;

beforeEach(async () => {
  adminId = await createAccount(ctx, ADMIN, { role: 'admin' });
  playerId = await createAccount(ctx, PLAYER);
  adminToken = (await login(ctx, ADMIN)).accessToken;
});

const asAdmin = () => {
  const api = ctx.api();
  return {
    post: (path: string, body?: object) => api.post(path).set(bearer(adminToken)).send(body),
    put: (path: string, body?: object) => api.put(path).set(bearer(adminToken)).send(body),
    get: (path: string) => api.get(path).set(bearer(adminToken)),
    delete: (path: string) => api.delete(path).set(bearer(adminToken)),
  };
};
const tryLogin = () => ctx.api().post('/v1/auth/login').send({ email: PLAYER, password: 'correct horse battery' });

type Route = [method: 'get' | 'post' | 'put' | 'delete', path: string];

// Open to GMs (on player targets only), tested in admin-accounts.test.ts.
const STAFF_ROUTES: Route[] = [
  ['get', '/v1/admin/accounts'],
  ['get', '/v1/admin/accounts/1'],
  ['post', '/v1/admin/accounts/1/ban'],
  ['delete', '/v1/admin/accounts/1/ban'],
];

const ADMIN_ROUTES: Route[] = [
  ['post', '/v1/admin/accounts'],
  ['delete', '/v1/admin/accounts/1'],
  ['post', '/v1/admin/accounts/1/verify-email'],
  ['post', '/v1/admin/accounts/1/password-reset'],
  ['post', '/v1/admin/accounts/1/unlock'],
  ['post', '/v1/admin/accounts/1/revoke-sessions'],
  ['put', '/v1/admin/accounts/1/role'],
  ['get', '/v1/admin/realms'],
  ['post', '/v1/admin/realms'],
  ['get', '/v1/admin/realms/1'],
  ['put', '/v1/admin/realms/1'],
  ['delete', '/v1/admin/realms/1'],
];

describe('admin authorization', () => {
  it.each([...STAFF_ROUTES, ...ADMIN_ROUTES])('%s %s → 401 without a token', async (method, path) => {
    expectError(await ctx.api()[method](path), 401, 'unauthorized');
  });

  it.each(STAFF_ROUTES)('%s %s → 403 for player', async (method, path) => {
    const { accessToken } = await login(ctx, PLAYER);
    expectError(await ctx.api()[method](path).set(bearer(accessToken)).send({}), 403, 'forbidden');
  });

  it.each(ADMIN_ROUTES)('%s %s → 403 for player and gm', async (method, path) => {
    await createAccount(ctx, 'gm@example.com', { role: 'gm' });
    for (const email of [PLAYER, 'gm@example.com']) {
      const { accessToken } = await login(ctx, email);
      expectError(await ctx.api()[method](path).set(bearer(accessToken)).send({}), 403, 'forbidden');
    }
  });
});

describe('bans', () => {
  const ban = (body: object, id = playerId) => asAdmin().post(`/v1/admin/accounts/${id}/ban`, body);

  it('a permanent ban blocks login and refresh with the reason, and unban restores login', async () => {
    const { refreshToken } = await login(ctx, PLAYER);
    const res = await ban({ reason: 'Gold botting' });
    expect(res.status).toBe(201);
    expect(res.body).toMatchObject({ accountId: playerId, reason: 'Gold botting', expiresAt: null, bannedBy: adminId });

    const loginErr = expectError(await tryLogin(), 403, 'account_banned');
    expect(loginErr.details).toEqual({ reason: 'Gold botting', expiresAt: null });
    const refreshErr = expectError(
      await ctx.api().post('/v1/auth/refresh').send({ refreshToken }),
      403,
      'account_banned',
    );
    expect(refreshErr.details).toEqual({ reason: 'Gold botting', expiresAt: null });

    expect((await asAdmin().delete(`/v1/admin/accounts/${playerId}/ban`)).status).toBe(204);
    expect((await tryLogin()).status).toBe(200);
    // Banning ended the old sessions for good.
    expectError(await ctx.api().post('/v1/auth/refresh').send({ refreshToken }), 401, 'invalid_refresh_token');
  });

  it('a timed ban stops blocking once it expires', async () => {
    const expiresAt = new Date(ctx.clock.now().getTime() + DAY).toISOString();
    expect((await ban({ reason: 'Spam', expiresAt })).body.expiresAt).toBe(expiresAt);
    expect(expectError(await tryLogin(), 403, 'account_banned').details).toEqual({ reason: 'Spam', expiresAt });
    ctx.clock.advance(DAY);
    expect((await tryLogin()).status).toBe(200);
  });

  it('a new ban replaces the active one', async () => {
    await ban({ reason: 'First', expiresAt: new Date(ctx.clock.now().getTime() + HOUR).toISOString() });
    await ban({ reason: 'Second' });
    expect(expectError(await tryLogin(), 403, 'account_banned').details).toEqual({ reason: 'Second', expiresAt: null });
    ctx.clock.advance(2 * HOUR);
    expectError(await tryLogin(), 403, 'account_banned');
  });

  it('rejects bad input, unknown accounts and self-bans', async () => {
    expectError(await ban({}), 400, 'validation_error');
    expectError(await ban({ reason: 'x', expiresAt: 'tomorrow' }), 400, 'validation_error');
    expectError(await ban({ reason: 'x', expiresAt: ctx.clock.now().toISOString() }), 400, 'validation_error');
    expectError(await ban({ reason: 'x' }, 999_999), 404, 'account_not_found');
    expectError(await ban({ reason: 'x' }, adminId), 400, 'cannot_modify_self');
    expectError(await asAdmin().post('/v1/admin/accounts/abc/ban', { reason: 'x' }), 400, 'validation_error');
    expectError(await asAdmin().delete('/v1/admin/accounts/999999/ban'), 404, 'account_not_found');
  });
});

describe('roles', () => {
  it('a role change shows up in the next issued JWT', async () => {
    const res = await asAdmin().put(`/v1/admin/accounts/${playerId}/role`, { role: 'gm' });
    expect(res.status).toBe(200);
    expect(res.body).toMatchObject({ id: playerId, email: PLAYER, role: 'gm' });
    const { accessToken } = await login(ctx, PLAYER);
    const payload = JSON.parse(Buffer.from(accessToken.split('.')[1]!, 'base64url').toString());
    expect(payload.role).toBe('gm');
    expect((await ctx.api().get('/v1/me').set(bearer(accessToken))).body.role).toBe('gm');
  });

  it('a promoted admin can use admin routes with a fresh token', async () => {
    await asAdmin().put(`/v1/admin/accounts/${playerId}/role`, { role: 'admin' });
    const { accessToken } = await login(ctx, PLAYER);
    expect((await ctx.api().get('/v1/admin/realms').set(bearer(accessToken))).status).toBe(200);
  });

  it('rejects unknown roles, unknown accounts and self-changes', async () => {
    expectError(await asAdmin().put(`/v1/admin/accounts/${playerId}/role`, { role: 'god' }), 400, 'validation_error');
    expectError(await asAdmin().put('/v1/admin/accounts/999999/role', { role: 'gm' }), 404, 'account_not_found');
    expectError(await asAdmin().put(`/v1/admin/accounts/${adminId}/role`, { role: 'player' }), 400, 'cannot_modify_self');
  });
});

describe('realms', () => {
  const realm = {
    name: 'Emberfall',
    host: 'eu1.chthonic.example',
    port: 7000,
    adminHost: null,
    adminPort: null,
    region: 'eu-west',
    status: 'online',
    sortOrder: 2,
  };

  it('supports full CRUD for admins', async () => {
    const created = await asAdmin().post('/v1/admin/realms', realm);
    expect(created.status).toBe(201);
    expect(created.body).toEqual({ id: expect.any(Number), ...realm });
    const id = created.body.id;

    expect((await asAdmin().get(`/v1/admin/realms/${id}`)).body).toEqual(created.body);

    const updated = await asAdmin().put(`/v1/admin/realms/${id}`, { ...realm, status: 'maintenance', port: 7001 });
    expect(updated.status).toBe(200);
    expect(updated.body).toMatchObject({ id, status: 'maintenance', port: 7001 });
    expect((await asAdmin().get('/v1/admin/realms')).body.realms).toEqual([updated.body]);

    expect((await asAdmin().delete(`/v1/admin/realms/${id}`)).status).toBe(204);
    expectError(await asAdmin().get(`/v1/admin/realms/${id}`), 404, 'realm_not_found');
    expectError(await asAdmin().put(`/v1/admin/realms/${id}`, realm), 404, 'realm_not_found');
    expectError(await asAdmin().delete(`/v1/admin/realms/${id}`), 404, 'realm_not_found');
  });

  it('defaults status to offline and sortOrder to 0', async () => {
    const { status, sortOrder } = (await asAdmin().post('/v1/admin/realms', { ...realm, status: undefined, sortOrder: undefined })).body;
    expect({ status, sortOrder }).toEqual({ status: 'offline', sortOrder: 0 });
  });

  it('rejects duplicate names and invalid fields', async () => {
    await asAdmin().post('/v1/admin/realms', realm);
    expectError(await asAdmin().post('/v1/admin/realms', realm), 409, 'realm_name_taken');
    const other = (await asAdmin().post('/v1/admin/realms', { ...realm, name: 'Other' })).body;
    expectError(await asAdmin().put(`/v1/admin/realms/${other.id}`, realm), 409, 'realm_name_taken');
    for (const bad of [{ port: 0 }, { port: 70000 }, { status: 'busy' }, { name: '' }, { host: undefined }, { port: '7000' }]) {
      expectError(await asAdmin().post('/v1/admin/realms', { ...realm, name: 'New', ...bad }), 400, 'validation_error');
    }
  });

  it('GET /v1/realms requires auth and returns realms by sort order', async () => {
    for (const [name, sortOrder] of [['Charlie', 3], ['Alpha', 1], ['Bravo', 2]] as const) {
      await asAdmin().post('/v1/admin/realms', { ...realm, name, sortOrder });
    }
    expectError(await ctx.api().get('/v1/realms'), 401, 'unauthorized');
    const { accessToken } = await login(ctx, PLAYER);
    const res = await ctx.api().get('/v1/realms').set(bearer(accessToken));
    expect(res.status).toBe(200);
    expect(res.body.realms.map((r: { name: string }) => r.name)).toEqual(['Alpha', 'Bravo', 'Charlie']);
  });

  it('admin realm CRUD reads and writes adminHost/adminPort; the public list hides them', async () => {
    const withAdmin = { ...realm, adminHost: 'world', adminPort: 7780 };
    const created = await asAdmin().post('/v1/admin/realms', withAdmin);
    expect(created.status).toBe(201);
    expect(created.body).toEqual({ id: expect.any(Number), ...withAdmin });
    expect((await asAdmin().get(`/v1/admin/realms/${created.body.id}`)).body).toEqual(created.body);
    expect((await asAdmin().get('/v1/admin/realms')).body.realms).toEqual([created.body]);

    const cleared = await asAdmin().put(`/v1/admin/realms/${created.body.id}`, realm);
    expect(cleared.body).toMatchObject({ adminHost: null, adminPort: null });
    await asAdmin().put(`/v1/admin/realms/${created.body.id}`, withAdmin);

    const { accessToken } = await login(ctx, PLAYER);
    const [pub] = (await ctx.api().get('/v1/realms').set(bearer(accessToken))).body.realms;
    expect(pub).toEqual({ id: created.body.id, name: 'Emberfall', host: 'eu1.chthonic.example', port: 7000, region: 'eu-west', status: 'online', sortOrder: 2 });
    expect(pub).not.toHaveProperty('adminHost');
    expect(pub).not.toHaveProperty('adminPort');
  });

  it('rejects an admin endpoint that is half set or invalid', async () => {
    for (const bad of [
      { adminHost: 'world' },
      { adminPort: 7780 },
      { adminHost: 'world', adminPort: 0 },
      { adminHost: 'world', adminPort: 70000 },
      { adminHost: '', adminPort: 7780 },
      { adminHost: 'world', adminPort: '7780' },
    ]) {
      expectError(await asAdmin().post('/v1/admin/realms', { ...realm, ...bad }), 400, 'validation_error');
    }
  });
});
