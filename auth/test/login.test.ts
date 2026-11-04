import { SignJWT, createLocalJWKSet, decodeProtectedHeader, jwtVerify } from 'jose';
import { describe, expect, it } from 'vitest';
import { createTokenService } from '../src/services/tokens.js';
import { MINUTE, PASSWORD, bearer, createAccount, expectError, generateKeys, login, useTestApp } from './helpers.js';

const ctx = useTestApp();
const email = 'player@example.com';
const tryLogin = (body: object) => ctx.api().post('/v1/auth/login').send(body);

describe('POST /v1/auth/login', () => {
  it('returns an RS256 access token that verifies against the published JWKS', async () => {
    const id = await createAccount(ctx, email, { role: 'gm' });
    const session = await login(ctx, email);
    expect(session).toMatchObject({ tokenType: 'Bearer', expiresIn: 900, refreshToken: expect.any(String) });
    expect(Buffer.from(session.refreshToken, 'base64url')).toHaveLength(32);
    expect(new Date(session.refreshTokenExpiresAt).getTime() - ctx.clock.now().getTime()).toBe(30 * 24 * 60 * MINUTE);

    const jwks = (await ctx.api().get('/.well-known/jwks.json')).body;
    const { payload, protectedHeader } = await jwtVerify(session.accessToken, createLocalJWKSet(jwks), {
      issuer: ctx.config.JWT_ISSUER,
      audience: ctx.config.JWT_AUDIENCE,
      currentDate: ctx.clock.now(),
    });
    expect(protectedHeader).toMatchObject({ alg: 'RS256', kid: ctx.keys.kid });
    expect(payload).toMatchObject({
      sub: String(id),
      email,
      role: 'gm',
      iss: ctx.config.JWT_ISSUER,
      aud: ctx.config.JWT_AUDIENCE,
    });
    expect(payload.exp! - payload.iat!).toBe(900);
    expect(new Date(session.accessTokenExpiresAt).getTime()).toBe(payload.exp! * 1000);
  });

  it('is case-insensitive on email', async () => {
    await createAccount(ctx, email);
    expect((await tryLogin({ email: 'PLAYER@Example.com', password: PASSWORD })).status).toBe(200);
  });

  it('gives identical invalid_credentials for unknown email and wrong password', async () => {
    await createAccount(ctx, email);
    const unknown = await tryLogin({ email: 'ghost@example.com', password: PASSWORD });
    const wrong = await tryLogin({ email, password: 'wrong password!' });
    expectError(unknown, 401, 'invalid_credentials');
    expect(wrong.status).toBe(unknown.status);
    expect(wrong.body).toEqual(unknown.body);
  });

  it('only reveals email_not_verified to someone with the right password', async () => {
    await createAccount(ctx, email, { verified: false });
    expectError(await tryLogin({ email, password: 'wrong password!' }), 401, 'invalid_credentials');
    expectError(await tryLogin({ email, password: PASSWORD }), 403, 'email_not_verified');
  });
});

describe('account lockout', () => {
  const failTimes = async (n: number) => {
    for (let i = 0; i < n; i++) expectError(await tryLogin({ email, password: 'wrong password!' }), 401, 'invalid_credentials');
  };

  it('locks after 5 failures, even for the right password, and unlocks after 15 minutes', async () => {
    await createAccount(ctx, email);
    await failTimes(5);
    const locked = expectError(await tryLogin({ email, password: PASSWORD }), 423, 'account_locked');
    expect(locked.details).toEqual({ lockedUntil: new Date(ctx.clock.now().getTime() + 15 * MINUTE).toISOString() });

    ctx.clock.advance(15 * MINUTE - 1000);
    expectError(await tryLogin({ email, password: PASSWORD }), 423, 'account_locked');
    ctx.clock.advance(1000);
    expect((await tryLogin({ email, password: PASSWORD })).status).toBe(200);
  });

  it('gives a fresh 5 attempts after the lock expires', async () => {
    await createAccount(ctx, email);
    await failTimes(5);
    ctx.clock.advance(15 * MINUTE);
    await failTimes(4);
    expect((await tryLogin({ email, password: PASSWORD })).status).toBe(200);
  });

  it('resets the counter on a successful login', async () => {
    await createAccount(ctx, email);
    await failTimes(4);
    await login(ctx, email);
    await failTimes(4);
    expect((await tryLogin({ email, password: PASSWORD })).status).toBe(200);
  });
});

describe('GET /v1/me', () => {
  it('returns the account for a valid token', async () => {
    const id = await createAccount(ctx, email);
    const { accessToken } = await login(ctx, email);
    const res = await ctx.api().get('/v1/me').set(bearer(accessToken));
    expect(res.status).toBe(200);
    expect(res.body).toEqual({
      id,
      email,
      role: 'player',
      emailVerified: true,
      createdAt: ctx.clock.now().toISOString(),
    });
  });

  it('rejects a missing, malformed, expired, tampered or foreign token with 401', async () => {
    await createAccount(ctx, email);
    const { accessToken } = await login(ctx, email);
    const me = (headers: Record<string, string> = {}) => ctx.api().get('/v1/me').set(headers);

    expectError(await me(), 401, 'unauthorized');
    expectError(await me({ Authorization: `Basic ${accessToken}` }), 401, 'unauthorized');
    expectError(await me(bearer('not.a.jwt')), 401, 'unauthorized');

    const [h, p, s] = accessToken.split('.') as [string, string, string];
    const claims = JSON.parse(Buffer.from(p, 'base64url').toString());
    const forged = Buffer.from(JSON.stringify({ ...claims, role: 'admin' })).toString('base64url');
    expectError(await me(bearer(`${h}.${forged}.${s}`)), 401, 'unauthorized');

    // Signed by a key that is not in our JWKS.
    const otherKeys = await generateKeys();
    const foreign = await createTokenService({ config: ctx.config, clock: ctx.clock, keys: otherKeys }).signAccessToken({
      id: 1,
      email,
      role: 'admin',
    });
    expectError(await me(bearer(foreign.token)), 401, 'unauthorized');

    // Right key, wrong audience.
    const wrongAud = await new SignJWT({ email, role: 'player' })
      .setProtectedHeader({ alg: 'RS256', kid: ctx.keys.kid })
      .setSubject(claims.sub)
      .setIssuer(ctx.config.JWT_ISSUER)
      .setAudience('someone-else')
      .setIssuedAt(claims.iat)
      .setExpirationTime(claims.exp)
      .sign(ctx.keys.privateKey);
    expectError(await me(bearer(wrongAud)), 401, 'unauthorized');

    expect((await me(bearer(accessToken))).status).toBe(200);
    ctx.clock.advance(15 * MINUTE);
    expectError(await me(bearer(accessToken)), 401, 'unauthorized');
  });

  it('carries the kid header that matches the JWKS', async () => {
    await createAccount(ctx, email);
    const { accessToken } = await login(ctx, email);
    expect(decodeProtectedHeader(accessToken).kid).toBe(ctx.keys.kid);
  });
});
