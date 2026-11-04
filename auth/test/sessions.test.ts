import type { RowDataPacket } from 'mysql2/promise';
import { describe, expect, it } from 'vitest';
import { DAY, bearer, createAccount, expectError, login, useTestApp } from './helpers.js';

const ctx = useTestApp();
const email = 'player@example.com';
const refresh = (refreshToken: string) => ctx.api().post('/v1/auth/refresh').send({ refreshToken });
const logout = (refreshToken: string) => ctx.api().post('/v1/auth/logout').send({ refreshToken });

describe('POST /v1/auth/refresh', () => {
  it('rotates the refresh token and issues a new working pair', async () => {
    await createAccount(ctx, email);
    const first = await login(ctx, email);
    ctx.clock.advance(1000);
    const res = await refresh(first.refreshToken);
    expect(res.status).toBe(200);
    expect(res.body.refreshToken).not.toBe(first.refreshToken);
    expect(res.body.accessToken).not.toBe(first.accessToken);
    expect((await ctx.api().get('/v1/me').set(bearer(res.body.accessToken))).status).toBe(200);
  });

  it('stores refresh tokens only as SHA-256 hashes', async () => {
    await createAccount(ctx, email);
    const { refreshToken } = await login(ctx, email);
    const [rows] = await ctx.pool.query<RowDataPacket[]>('SELECT token_hash FROM refresh_tokens');
    expect(rows[0]?.token_hash).toMatch(/^[0-9a-f]{64}$/);
    expect(JSON.stringify(rows)).not.toContain(refreshToken);
  });

  it('revokes the whole family when a rotated token is reused', async () => {
    await createAccount(ctx, email);
    const first = await login(ctx, email);
    const second = (await refresh(first.refreshToken)).body;
    const third = (await refresh(second.refreshToken)).body;

    expectError(await refresh(first.refreshToken), 401, 'invalid_refresh_token');
    // The newest token in the family is dead too.
    expectError(await refresh(third.refreshToken), 401, 'invalid_refresh_token');
  });

  it('does not touch other sessions of the same account on reuse', async () => {
    await createAccount(ctx, email);
    const sessionA = await login(ctx, email);
    const sessionB = await login(ctx, email);
    await refresh(sessionA.refreshToken);
    await refresh(sessionA.refreshToken); // reuse → family A revoked
    expect((await refresh(sessionB.refreshToken)).status).toBe(200);
  });

  it('rejects a refresh token after 30 days', async () => {
    await createAccount(ctx, email);
    const { refreshToken } = await login(ctx, email);
    ctx.clock.advance(30 * DAY);
    expectError(await refresh(refreshToken), 401, 'invalid_refresh_token');
  });

  it('issues tokens with the current role', async () => {
    const id = await createAccount(ctx, email);
    const { refreshToken } = await login(ctx, email);
    await ctx.pool.query("UPDATE accounts SET role = 'gm' WHERE id = ?", [id]);
    const res = await refresh(refreshToken);
    const payload = JSON.parse(Buffer.from(res.body.accessToken.split('.')[1], 'base64url').toString());
    expect(payload.role).toBe('gm');
  });

  it('rejects unknown tokens and invalid bodies', async () => {
    expectError(await refresh('garbage'), 401, 'invalid_refresh_token');
    expectError(await ctx.api().post('/v1/auth/refresh').send({}), 400, 'validation_error');
  });
});

describe('POST /v1/auth/logout', () => {
  it('revokes the session so its refresh token stops working', async () => {
    await createAccount(ctx, email);
    const session = await login(ctx, email);
    const rotated = (await refresh(session.refreshToken)).body;
    expect((await logout(rotated.refreshToken)).status).toBe(204);
    expectError(await refresh(rotated.refreshToken), 401, 'invalid_refresh_token');
  });

  it('is idempotent and silent for unknown tokens', async () => {
    expect((await logout('never-issued')).status).toBe(204);
  });

  it('rejects an invalid body', async () => {
    expectError(await ctx.api().post('/v1/auth/logout').send({ refreshToken: '' }), 400, 'validation_error');
  });
});
