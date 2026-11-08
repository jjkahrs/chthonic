import type { RowDataPacket } from 'mysql2/promise';
import { describe, expect, it } from 'vitest';
import { HOUR, MINUTE, PASSWORD, createAccount, expectError, login, useTestApp } from './helpers.js';

const ctx = useTestApp();
const email = 'player@example.com';
const NEW_PASSWORD = 'a brand new password';

const forgot = (e: string) => ctx.api().post('/v1/auth/forgot-password').send({ email: e });
const reset = (token: string, password = NEW_PASSWORD) =>
  ctx.api().post('/v1/auth/reset-password').send({ token, password });
const tryLogin = (password: string) => ctx.api().post('/v1/auth/login').send({ email, password });

describe('POST /v1/auth/forgot-password', () => {
  it('responds identically for known and unknown emails, mailing only the real account', async () => {
    await createAccount(ctx, email);
    const known = await forgot(email);
    const unknown = await forgot('ghost@example.com');
    expect(known.status).toBe(202);
    expect(unknown.status).toBe(known.status);
    expect(unknown.body).toEqual(known.body);
    expect(ctx.mailer.sent.map((m) => m.to)).toEqual([email]);
    expect(ctx.mailer.sent[0]?.text).toContain('http://chthonic.test/account/reset-password?token=');
  });

  it('rejects an invalid email', async () => {
    expectError(await forgot('nope'), 400, 'validation_error');
  });
});

describe('POST /v1/auth/reset-password', () => {
  it('sets the new password and the token works only once', async () => {
    await createAccount(ctx, email);
    await forgot(email);
    const token = ctx.mailer.tokenFor(email);

    expect((await reset(token)).status).toBe(200);
    expectError(await tryLogin(PASSWORD), 401, 'invalid_credentials');
    expect((await tryLogin(NEW_PASSWORD)).status).toBe(200);
    expectError(await reset(token, 'yet another password'), 400, 'invalid_token');
  });

  it('expires the token after 1 hour', async () => {
    await createAccount(ctx, email);
    await forgot(email);
    const token = ctx.mailer.tokenFor(email);
    ctx.clock.advance(HOUR);
    expectError(await reset(token), 400, 'invalid_token');
  });

  it('accepts the token just before 1 hour', async () => {
    await createAccount(ctx, email);
    await forgot(email);
    const token = ctx.mailer.tokenFor(email);
    ctx.clock.advance(HOUR - MINUTE);
    expect((await reset(token)).status).toBe(200);
  });

  it('invalidates older reset links once one is used', async () => {
    await createAccount(ctx, email);
    await forgot(email);
    const older = ctx.mailer.tokenFor(email);
    await forgot(email);
    expect((await reset(ctx.mailer.tokenFor(email))).status).toBe(200);
    expectError(await reset(older), 400, 'invalid_token');
  });

  it('revokes every existing refresh token', async () => {
    await createAccount(ctx, email);
    const a = await login(ctx, email);
    const b = await login(ctx, email);
    await forgot(email);
    await reset(ctx.mailer.tokenFor(email));
    for (const s of [a, b]) {
      const res = await ctx.api().post('/v1/auth/refresh').send({ refreshToken: s.refreshToken });
      expectError(res, 401, 'invalid_refresh_token');
    }
  });

  it('clears a lockout and verifies the email', async () => {
    await createAccount(ctx, email, { verified: false });
    for (let i = 0; i < 5; i++) await tryLogin('wrong password!');
    expectError(await tryLogin(PASSWORD), 423, 'account_locked');
    await forgot(email);
    await reset(ctx.mailer.tokenFor(email));
    expect((await tryLogin(NEW_PASSWORD)).status).toBe(200);
  });

  it('clears the lockout on every IP', async () => {
    ctx.restart({ config: { ...ctx.config, TRUST_PROXY: 'true' } });
    await createAccount(ctx, email);
    const from = (ip: string, password: string) =>
      ctx.api().post('/v1/auth/login').set('X-Forwarded-For', ip).send({ email, password });
    for (const ip of ['203.0.113.1', '203.0.113.2']) {
      for (let i = 0; i < 5; i++) await from(ip, 'wrong password!');
      expectError(await from(ip, PASSWORD), 423, 'account_locked');
    }
    await forgot(email);
    await reset(ctx.mailer.tokenFor(email));
    const [rows] = await ctx.pool.query<RowDataPacket[]>('SELECT COUNT(*) AS n FROM login_failures');
    expect(rows[0]?.n).toBe(0);
    for (const ip of ['203.0.113.1', '203.0.113.2']) expect((await from(ip, NEW_PASSWORD)).status).toBe(200);
  });

  it('enforces the password policy and rejects bad tokens', async () => {
    expectError(await reset('whatever', 'short'), 400, 'validation_error');
    expectError(await reset('not-a-real-token'), 400, 'invalid_token');
  });
});
