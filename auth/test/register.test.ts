import type { RowDataPacket } from 'mysql2/promise';
import { describe, expect, it, vi } from 'vitest';
import { DAY, HOUR, PASSWORD, createAccount, expectError, useTestApp } from './helpers.js';

const ctx = useTestApp();
const email = 'new.player@example.com';

const register = (body: object) => ctx.api().post('/v1/auth/register').send(body);
const verify = (token: string) => ctx.api().post('/v1/auth/verify-email').send({ token });
const login = (password: string) => ctx.api().post('/v1/auth/login').send({ email, password });

describe('POST /v1/auth/register', () => {
  it('creates an unverified account and emails a verification link', async () => {
    const res = await register({ email, password: PASSWORD });
    expect(res.status).toBe(202);
    expect(res.body.message).toEqual(expect.any(String));

    const [mail] = ctx.mailer.to(email);
    expect(mail?.subject).toMatch(/verify/i);
    expect(mail?.text).toContain('http://chthonic.test/account/verify-email?token=');

    const [rows] = await ctx.pool.query<RowDataPacket[]>('SELECT role, email_verified_at, password_hash FROM accounts');
    expect(rows).toHaveLength(1);
    expect(rows[0]).toMatchObject({ role: 'player', email_verified_at: null });
    expect(rows[0]?.password_hash).toMatch(/^\$2b\$12\$/);
  });

  it('stores only a hash of the verification token', async () => {
    await register({ email, password: PASSWORD });
    const token = ctx.mailer.tokenFor(email);
    const [rows] = await ctx.pool.query<RowDataPacket[]>('SELECT token_hash FROM email_tokens');
    expect(rows[0]?.token_hash).toMatch(/^[0-9a-f]{64}$/);
    expect(rows[0]?.token_hash).not.toBe(token);
  });

  it('normalizes email case and whitespace', async () => {
    await register({ email: '  New.Player@EXAMPLE.com ', password: PASSWORD });
    const [rows] = await ctx.pool.query<RowDataPacket[]>('SELECT email FROM accounts');
    expect(rows[0]?.email).toBe(email);
  });

  it('answers a taken email exactly like a new one and notifies the owner', async () => {
    await createAccount(ctx, email);
    const fresh = await register({ email: 'someone.else@example.com', password: PASSWORD });
    const taken = await register({ email: email.toUpperCase(), password: PASSWORD });
    expect(taken.status).toBe(fresh.status);
    expect(taken.body).toEqual(fresh.body);
    expect(ctx.mailer.to(email).at(-1)?.subject).toMatch(/sign-up attempt/i);
    const [rows] = await ctx.pool.query<RowDataPacket[]>('SELECT COUNT(*) AS n FROM accounts WHERE email = ?', [email]);
    expect(rows[0]?.n).toBe(1);
  });

  it('re-registering an unverified email: only the latest password works and old links die', async () => {
    await register({ email, password: 'first registrant pw' });
    const oldToken = ctx.mailer.tokenFor(email);
    await register({ email, password: PASSWORD });
    const newToken = ctx.mailer.tokenFor(email);
    expect(newToken).not.toBe(oldToken);

    expectError(await verify(oldToken), 400, 'invalid_token');
    expect((await verify(newToken)).status).toBe(200);
    expectError(await login('first registrant pw'), 401, 'invalid_credentials');
    expect((await login(PASSWORD)).status).toBe(200);
    const [rows] = await ctx.pool.query<RowDataPacket[]>('SELECT COUNT(*) AS n FROM accounts');
    expect(rows[0]?.n).toBe(1);
  });

  it('leaves a verified account untouched when its email is re-registered', async () => {
    await createAccount(ctx, email);
    await register({ email, password: 'someone else pw' });
    expect(ctx.mailer.to(email).map((m) => m.subject)).toEqual(['Chthonic account sign-up attempt']);
    expect((await login(PASSWORD)).status).toBe(200);
    expectError(await login('someone else pw'), 401, 'invalid_credentials');
  });

  it('answers new, unverified and verified emails identically', async () => {
    await createAccount(ctx, 'verified@example.com');
    await createAccount(ctx, 'unverified@example.com', { verified: false });
    const responses = [];
    for (const e of ['brand.new@example.com', 'unverified@example.com', 'verified@example.com']) {
      const res = await register({ email: e, password: PASSWORD });
      responses.push({ status: res.status, body: res.body, type: res.headers['content-type'] });
    }
    expect(responses[0]?.status).toBe(202);
    expect(new Set(responses.map((r) => JSON.stringify(r))).size).toBe(1);
  });

  it('takes the same path after losing the duplicate-key race', async () => {
    await register({ email, password: 'first registrant pw' });
    const oldToken = ctx.mailer.tokenFor(email);
    // Make the first email lookup miss, so the insert hits the unique key as if a concurrent request won.
    const realQuery = ctx.pool.query.bind(ctx.pool);
    let missed = false;
    const spy = vi.spyOn(ctx.pool, 'query').mockImplementation(((sql: string, params?: unknown[]) => {
      if (!missed && sql.includes('WHERE email = ?')) {
        missed = true;
        return Promise.resolve([[], []]);
      }
      return realQuery(sql, params);
    }) as unknown as typeof ctx.pool.query);
    try {
      expect((await register({ email, password: PASSWORD })).status).toBe(202);
    } finally {
      spy.mockRestore();
    }
    expect(missed).toBe(true);
    expectError(await verify(oldToken), 400, 'invalid_token');
    expect((await verify(ctx.mailer.tokenFor(email))).status).toBe(200);
    expect((await login(PASSWORD)).status).toBe(200);
  });

  it.each([
    ['bad email', { email: 'not-an-email', password: PASSWORD }],
    ['short password', { email, password: 'x'.repeat(9) }],
    ['long password', { email, password: 'x'.repeat(129) }],
    ['missing password', { email }],
    ['non-string password', { email, password: 1234567890 }],
  ])('rejects %s with 400 validation_error', async (_name, body) => {
    expectError(await register(body), 400, 'validation_error');
  });

  it('accepts passwords at the 10 and 128 character bounds', async () => {
    expect((await register({ email: 'a@example.com', password: 'x'.repeat(10) })).status).toBe(202);
    expect((await register({ email: 'b@example.com', password: 'y'.repeat(128) })).status).toBe(202);
  });
});

describe('POST /v1/auth/verify-email', () => {
  it('blocks login before verification, then verifies once', async () => {
    await register({ email, password: PASSWORD });
    const loginBefore = await ctx.api().post('/v1/auth/login').send({ email, password: PASSWORD });
    expectError(loginBefore, 403, 'email_not_verified');

    const token = ctx.mailer.tokenFor(email);
    const first = await ctx.api().post('/v1/auth/verify-email').send({ token });
    expect(first.status).toBe(200);
    expectError(await ctx.api().post('/v1/auth/verify-email').send({ token }), 400, 'invalid_token');

    const loginAfter = await ctx.api().post('/v1/auth/login').send({ email, password: PASSWORD });
    expect(loginAfter.status).toBe(200);
  });

  it('rejects a token older than 24 hours', async () => {
    await register({ email, password: PASSWORD });
    const token = ctx.mailer.tokenFor(email);
    ctx.clock.advance(DAY + 1);
    expectError(await ctx.api().post('/v1/auth/verify-email').send({ token }), 400, 'invalid_token');
  });

  it('accepts a token just inside 24 hours', async () => {
    await register({ email, password: PASSWORD });
    const token = ctx.mailer.tokenFor(email);
    ctx.clock.advance(DAY - HOUR);
    expect((await ctx.api().post('/v1/auth/verify-email').send({ token })).status).toBe(200);
  });

  it('rejects unknown tokens and invalid bodies', async () => {
    expectError(await ctx.api().post('/v1/auth/verify-email').send({ token: 'nope' }), 400, 'invalid_token');
    expectError(await ctx.api().post('/v1/auth/verify-email').send({}), 400, 'validation_error');
  });
});

describe('POST /v1/auth/resend-verification', () => {
  const resend = (e: string) => ctx.api().post('/v1/auth/resend-verification').send({ email: e });

  it('gives the same answer for unknown, unverified and verified emails', async () => {
    await createAccount(ctx, 'unverified@example.com', { verified: false });
    await createAccount(ctx, 'verified@example.com');
    const responses = await Promise.all(
      ['nobody@example.com', 'unverified@example.com', 'verified@example.com'].map(async (e) => {
        const res = await resend(e);
        return { status: res.status, body: res.body };
      }),
    );
    expect(responses[0]?.status).toBe(202);
    expect(new Set(responses.map((r) => JSON.stringify(r))).size).toBe(1);
    expect(ctx.mailer.sent.map((m) => m.to)).toEqual(['unverified@example.com']);
  });

  it('sends a working new token', async () => {
    await createAccount(ctx, email, { verified: false });
    await resend(email);
    const res = await ctx.api().post('/v1/auth/verify-email').send({ token: ctx.mailer.tokenFor(email) });
    expect(res.status).toBe(200);
  });

  it('rejects an invalid email with 400 validation_error', async () => {
    expectError(await resend('nope'), 400, 'validation_error');
  });
});
