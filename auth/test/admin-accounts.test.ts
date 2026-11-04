import { beforeEach, describe, expect, it } from 'vitest';
import { PASSWORD, bearer, createAccount, expectError, login, useTestApp } from './helpers.js';

const ctx = useTestApp();
const ADMIN = 'admin@example.com';
const GM = 'gm@example.com';
const PLAYER = 'player@example.com';

let adminId: number;
let gmId: number;
let playerId: number;
let adminToken: string;
let gmToken: string;

beforeEach(async () => {
  adminId = await createAccount(ctx, ADMIN, { role: 'admin' });
  gmId = await createAccount(ctx, GM, { role: 'gm' });
  playerId = await createAccount(ctx, PLAYER);
  adminToken = (await login(ctx, ADMIN)).accessToken;
  gmToken = (await login(ctx, GM)).accessToken;
});

const as = (token: string) => {
  const api = ctx.api();
  return {
    get: (path: string) => api.get(path).set(bearer(token)),
    post: (path: string, body?: object) => api.post(path).set(bearer(token)).send(body),
    delete: (path: string) => api.delete(path).set(bearer(token)),
  };
};
const asAdmin = () => as(adminToken);
const asGm = () => as(gmToken);
const tryLogin = (email: string, password = PASSWORD) => ctx.api().post('/v1/auth/login').send({ email, password });
const emails = (body: { accounts: Array<{ email: string }> }) => body.accounts.map((a) => a.email);

describe('GET /v1/admin/accounts', () => {
  it('lists accounts by id with moderation state and the active ban', async () => {
    await asAdmin().post(`/v1/admin/accounts/${playerId}/ban`, { reason: 'Botting' });
    const res = await asAdmin().get('/v1/admin/accounts');
    expect(res.status).toBe(200);
    expect(res.body).toMatchObject({ total: 3, limit: 50, offset: 0 });
    expect(emails(res.body)).toEqual([ADMIN, GM, PLAYER]);
    expect(res.body.accounts[2]).toEqual({
      id: playerId,
      email: PLAYER,
      role: 'player',
      emailVerified: true,
      emailVerifiedAt: ctx.clock.now().toISOString(),
      failedLoginCount: 0,
      lockedUntil: null,
      createdAt: ctx.clock.now().toISOString(),
      activeBan: expect.objectContaining({ accountId: playerId, reason: 'Botting', expiresAt: null, bannedBy: adminId }),
    });
    expect(res.body.accounts[0].activeBan).toBeNull();
  });

  it('paginates with limit/offset and a total', async () => {
    for (let i = 0; i < 4; i++) await createAccount(ctx, `p${i}@example.com`);
    const page = await asAdmin().get('/v1/admin/accounts?limit=2&offset=2');
    expect(page.body).toMatchObject({ total: 7, limit: 2, offset: 2 });
    expect(emails(page.body)).toEqual([PLAYER, 'p0@example.com']);
    expect(emails((await asAdmin().get('/v1/admin/accounts?offset=100')).body)).toEqual([]);
  });

  it('searches by email substring or exact id', async () => {
    await createAccount(ctx, 'other@test.org');
    expect(emails((await asAdmin().get('/v1/admin/accounts?q=EXAMPLE.com')).body)).toEqual([ADMIN, GM, PLAYER]);
    expect(emails((await asAdmin().get('/v1/admin/accounts?q=test.org')).body)).toEqual(['other@test.org']);
    expect(emails((await asAdmin().get(`/v1/admin/accounts?q=${gmId}`)).body)).toEqual([GM]);
    // LIKE wildcards are matched literally.
    expect((await asAdmin().get('/v1/admin/accounts?q=%25')).body.total).toBe(0);
    expect((await asAdmin().get('/v1/admin/accounts?q=_')).body.total).toBe(0);
  });

  it('filters by role, banned, verified and locked', async () => {
    await createAccount(ctx, 'new@example.com', { verified: false });
    await asAdmin().post(`/v1/admin/accounts/${playerId}/ban`, { reason: 'x' });
    for (let i = 0; i < 5; i++) await tryLogin(GM, 'wrong password!!');

    const list = async (query: string) => emails((await asAdmin().get(`/v1/admin/accounts?${query}`)).body);
    expect(await list('role=player')).toEqual([PLAYER, 'new@example.com']);
    expect(await list('role=gm')).toEqual([GM]);
    expect(await list('banned=true')).toEqual([PLAYER]);
    expect(await list('banned=false')).toEqual([ADMIN, GM, 'new@example.com']);
    expect(await list('verified=false')).toEqual(['new@example.com']);
    expect(await list('verified=true&role=player')).toEqual([PLAYER]);
    expect(await list('locked=true')).toEqual([GM]);
    expect(await list('locked=false&banned=false')).toEqual([ADMIN, 'new@example.com']);
  });

  it('a ban that expired or was lifted is not active', async () => {
    await asAdmin().post(`/v1/admin/accounts/${playerId}/ban`, { reason: 'x' });
    await asAdmin().delete(`/v1/admin/accounts/${playerId}/ban`);
    const res = await asAdmin().get('/v1/admin/accounts?banned=true');
    expect(res.body.total).toBe(0);
  });

  it('rejects bad query parameters', async () => {
    for (const q of ['limit=0', 'limit=101', 'offset=-1', 'role=god', 'banned=yes', 'verified=1', 'limit=abc']) {
      expectError(await asAdmin().get(`/v1/admin/accounts?${q}`), 400, 'validation_error');
    }
    expect((await asAdmin().get('/v1/admin/accounts?limit=100')).status).toBe(200);
  });

  it('GMs see only player accounts', async () => {
    expect(emails((await asGm().get('/v1/admin/accounts')).body)).toEqual([PLAYER]);
    expect(emails((await asGm().get('/v1/admin/accounts?role=player')).body)).toEqual([PLAYER]);
    expect((await asGm().get(`/v1/admin/accounts?q=${adminId}`)).body.total).toBe(0);
    expectError(await asGm().get('/v1/admin/accounts?role=admin'), 403, 'forbidden');
    expectError(await asGm().get('/v1/admin/accounts?role=gm'), 403, 'forbidden');
  });
});

describe('GET /v1/admin/accounts/:id', () => {
  it('returns the account with its full ban history, newest first', async () => {
    await asAdmin().post(`/v1/admin/accounts/${playerId}/ban`, { reason: 'First' });
    ctx.clock.advance(1000);
    await asAdmin().delete(`/v1/admin/accounts/${playerId}/ban`);
    ctx.clock.advance(1000);
    await asAdmin().post(`/v1/admin/accounts/${playerId}/ban`, { reason: 'Second' });

    const res = await asAdmin().get(`/v1/admin/accounts/${playerId}`);
    expect(res.status).toBe(200);
    expect(res.body).toMatchObject({ id: playerId, email: PLAYER, role: 'player', activeBan: { reason: 'Second' } });
    expect(res.body.bans).toHaveLength(2);
    expect(res.body.bans[0]).toMatchObject({ reason: 'Second', liftedAt: null, liftedBy: null });
    expect(res.body.bans[1]).toMatchObject({
      reason: 'First',
      bannedBy: adminId,
      liftedAt: new Date(ctx.clock.now().getTime() - 1000).toISOString(),
      liftedBy: adminId,
    });
  });

  it('404s unknown accounts and 400s bad ids', async () => {
    expectError(await asAdmin().get('/v1/admin/accounts/999999'), 404, 'account_not_found');
    expectError(await asAdmin().get('/v1/admin/accounts/abc'), 400, 'validation_error');
  });

  it('GMs can view players but not GMs or admins', async () => {
    expect((await asGm().get(`/v1/admin/accounts/${playerId}`)).status).toBe(200);
    expectError(await asGm().get(`/v1/admin/accounts/${adminId}`), 403, 'forbidden');
    expectError(await asGm().get(`/v1/admin/accounts/${gmId}`), 403, 'forbidden');
  });
});

describe('POST /v1/admin/accounts', () => {
  it('creates a verified account that can log in at once', async () => {
    const res = await asAdmin().post('/v1/admin/accounts', { email: ' New.GM@Example.com ', password: PASSWORD, role: 'gm' });
    expect(res.status).toBe(201);
    expect(res.body).toMatchObject({
      id: expect.any(Number),
      email: 'new.gm@example.com',
      role: 'gm',
      emailVerified: true,
      activeBan: null,
    });
    expect(ctx.mailer.sent).toEqual([]);
    expect((await tryLogin('new.gm@example.com')).status).toBe(200);
  });

  it('defaults the role to player', async () => {
    expect((await asAdmin().post('/v1/admin/accounts', { email: 'p2@example.com', password: PASSWORD })).body.role).toBe(
      'player',
    );
  });

  it('409s a duplicate email, case-insensitively', async () => {
    expectError(await asAdmin().post('/v1/admin/accounts', { email: 'PLAYER@example.com', password: PASSWORD }), 409, 'email_taken');
  });

  it('validates email, password length and role', async () => {
    for (const bad of [
      {},
      { email: 'not-an-email', password: PASSWORD },
      { email: 'a@example.com', password: 'short' },
      { email: 'a@example.com', password: 'x'.repeat(129) },
      { email: 'a@example.com', password: PASSWORD, role: 'god' },
    ]) {
      expectError(await asAdmin().post('/v1/admin/accounts', bad), 400, 'validation_error');
    }
  });
});

describe('DELETE /v1/admin/accounts/:id', () => {
  it('hard-deletes the account with its bans and sessions', async () => {
    const { refreshToken } = await login(ctx, PLAYER);
    await asAdmin().post(`/v1/admin/accounts/${playerId}/ban`, { reason: 'x' });
    expect((await asAdmin().delete(`/v1/admin/accounts/${playerId}`)).status).toBe(204);

    expectError(await asAdmin().get(`/v1/admin/accounts/${playerId}`), 404, 'account_not_found');
    expectError(await tryLogin(PLAYER), 401, 'invalid_credentials');
    expectError(await ctx.api().post('/v1/auth/refresh').send({ refreshToken }), 401, 'invalid_refresh_token');
    const [bans] = await ctx.pool.query('SELECT id FROM bans WHERE account_id = ?', [playerId]);
    expect(bans).toEqual([]);
  });

  it('keeps bans the deleted admin issued, with bannedBy cleared', async () => {
    const otherAdmin = await createAccount(ctx, 'admin2@example.com', { role: 'admin' });
    const token = (await login(ctx, 'admin2@example.com')).accessToken;
    await as(token).post(`/v1/admin/accounts/${playerId}/ban`, { reason: 'x' });
    expect((await asAdmin().delete(`/v1/admin/accounts/${otherAdmin}`)).status).toBe(204);
    expect((await asAdmin().get(`/v1/admin/accounts/${playerId}`)).body.activeBan).toMatchObject({ bannedBy: null });
  });

  it('refuses self-delete and 404s unknown accounts', async () => {
    expectError(await asAdmin().delete(`/v1/admin/accounts/${adminId}`), 400, 'cannot_modify_self');
    expectError(await asAdmin().delete('/v1/admin/accounts/999999'), 404, 'account_not_found');
    expectError(await asAdmin().delete('/v1/admin/accounts/0'), 400, 'validation_error');
  });
});

describe('account actions', () => {
  it('verify-email lets an unverified account log in', async () => {
    const id = await createAccount(ctx, 'new@example.com', { verified: false });
    expectError(await tryLogin('new@example.com'), 403, 'email_not_verified');
    expect((await asAdmin().post(`/v1/admin/accounts/${id}/verify-email`)).status).toBe(204);
    expect((await tryLogin('new@example.com')).status).toBe(200);
    expect((await asAdmin().get(`/v1/admin/accounts/${id}`)).body.emailVerifiedAt).toBe(ctx.clock.now().toISOString());
  });

  it('password-reset sends the normal reset email, whose token works', async () => {
    expect((await asAdmin().post(`/v1/admin/accounts/${playerId}/password-reset`)).status).toBe(204);
    const [mail] = ctx.mailer.to(PLAYER);
    expect(mail?.subject).toBe('Reset your Chthonic password');
    const token = ctx.mailer.tokenFor(PLAYER);
    const reset = await ctx.api().post('/v1/auth/reset-password').send({ token, password: 'a brand new password' });
    expect(reset.status).toBe(200);
    expect((await tryLogin(PLAYER, 'a brand new password')).status).toBe(200);
  });

  it('unlock clears the lockout and failure count', async () => {
    for (let i = 0; i < 5; i++) await tryLogin(PLAYER, 'wrong password!!');
    expectError(await tryLogin(PLAYER), 423, 'account_locked');
    expect((await asAdmin().post(`/v1/admin/accounts/${playerId}/unlock`)).status).toBe(204);
    expect((await asAdmin().get(`/v1/admin/accounts/${playerId}`)).body).toMatchObject({
      failedLoginCount: 0,
      lockedUntil: null,
    });
    expect((await tryLogin(PLAYER)).status).toBe(200);
  });

  it('revoke-sessions ends every refresh token', async () => {
    const a = await login(ctx, PLAYER);
    const b = await login(ctx, PLAYER);
    expect((await asAdmin().post(`/v1/admin/accounts/${playerId}/revoke-sessions`)).status).toBe(204);
    for (const { refreshToken } of [a, b]) {
      expectError(await ctx.api().post('/v1/auth/refresh').send({ refreshToken }), 401, 'invalid_refresh_token');
    }
    expect((await tryLogin(PLAYER)).status).toBe(200);
  });

  it.each(['verify-email', 'password-reset', 'unlock', 'revoke-sessions'])('%s 404s unknown accounts', async (action) => {
    expectError(await asAdmin().post(`/v1/admin/accounts/999999/${action}`), 404, 'account_not_found');
    expectError(await asAdmin().post(`/v1/admin/accounts/abc/${action}`), 400, 'validation_error');
  });
});

describe('GM bans', () => {
  it('a GM can ban and unban a player', async () => {
    const res = await asGm().post(`/v1/admin/accounts/${playerId}/ban`, { reason: 'Spam' });
    expect(res.status).toBe(201);
    expect(res.body).toMatchObject({ accountId: playerId, bannedBy: gmId });
    expectError(await tryLogin(PLAYER), 403, 'account_banned');
    expect((await asGm().delete(`/v1/admin/accounts/${playerId}/ban`)).status).toBe(204);
    expect((await tryLogin(PLAYER)).status).toBe(200);
  });

  it('a GM cannot ban or unban GMs or admins', async () => {
    const otherGm = await createAccount(ctx, 'gm2@example.com', { role: 'gm' });
    for (const id of [adminId, otherGm]) {
      expectError(await asGm().post(`/v1/admin/accounts/${id}/ban`, { reason: 'x' }), 403, 'forbidden');
      expectError(await asGm().delete(`/v1/admin/accounts/${id}/ban`), 403, 'forbidden');
    }
    // Nothing was written.
    expect((await asAdmin().get(`/v1/admin/accounts/${otherGm}`)).body.bans).toEqual([]);
  });

  it('a GM still gets 404 for unknown accounts and 400 for self', async () => {
    expectError(await asGm().post('/v1/admin/accounts/999999/ban', { reason: 'x' }), 404, 'account_not_found');
    expectError(await asGm().post(`/v1/admin/accounts/${gmId}/ban`, { reason: 'x' }), 400, 'cannot_modify_self');
  });
});
