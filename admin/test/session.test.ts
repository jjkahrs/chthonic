import { mkdtemp, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import express from 'express';
import supertest from 'supertest';
import { describe, expect, it } from 'vitest';
import { createAuthClient } from '../src/authClient.js';
import { createApp, type AppDeps } from '../src/http/app.js';
import { COOKIES, CSRF_HEADER, cookieWriter } from '../src/http/middleware.js';
import { RECHECK_MS } from '../src/services/session.js';
import { ADMIN, GM, MINUTE, PASSWORD, PLAYER, cookieFrom, expectError, testConfig, useTestApp } from './helpers.js';

const ctx = useTestApp();

const setCookies = (res: supertest.Response): string[] => [res.headers['set-cookie'] ?? []].flat() as string[];

async function csrfAgent() {
  const agent = ctx.api();
  const first = await agent.get('/api/session');
  return { agent, csrf: cookieFrom(first, COOKIES.csrf)! };
}

describe('login', () => {
  it('signs in admins and GMs and reports {id, email, role}', async () => {
    for (const [email, role, id] of [
      [ADMIN, 'admin', ctx.ids.admin],
      [GM, 'gm', ctx.ids.gm],
    ] as const) {
      const client = await ctx.login(email);
      expect((await client.get('/api/session')).body).toEqual({ id, email, role });
    }
  });

  it('rejects players with 403 and sets no session cookies', async () => {
    const { agent, csrf } = await csrfAgent();
    const res = await agent.post('/api/session/login').set(CSRF_HEADER, csrf).send({ email: PLAYER, password: PASSWORD });
    expectError(res, 403, 'forbidden');
    expect(cookieFrom(res, COOKIES.access)).toBeUndefined();
    expectError(await agent.get('/api/session'), 401, 'unauthorized');
    // The refresh token auth issued during the attempt was revoked again.
    const [[row]] = await ctx.authPool.query<import('mysql2').RowDataPacket[]>(
      'SELECT COUNT(*) AS live FROM refresh_tokens WHERE account_id = ? AND revoked_at IS NULL',
      [ctx.ids.player],
    );
    expect(row!.live).toBe(0);
  });

  it('passes auth errors through (wrong password → 401 invalid_credentials) and validates input', async () => {
    const { agent, csrf } = await csrfAgent();
    const login = (body: object) => agent.post('/api/session/login').set(CSRF_HEADER, csrf).send(body);
    expectError(await login({ email: ADMIN, password: 'wrong password!' }), 401, 'invalid_credentials');
    expectError(await login({ email: 'not-an-email', password: 'x' }), 400, 'validation_error');
  });

  it('keeps tokens in httpOnly SameSite=Strict cookies and never in the body', async () => {
    const { agent, csrf } = await csrfAgent();
    const res = await agent.post('/api/session/login').set(CSRF_HEADER, csrf).send({ email: ADMIN, password: PASSWORD });
    expect(res.status).toBe(200);
    expect(JSON.stringify(res.body)).not.toMatch(/eyJ|refresh/i);
    for (const name of [COOKIES.access, COOKIES.refresh]) {
      const cookie = setCookies(res).find((c) => c.startsWith(`${name}=`))!;
      expect(cookie).toMatch(/; HttpOnly/);
      expect(cookie).toMatch(/; SameSite=Strict/);
      expect(cookie).toMatch(/; Path=\/api/);
    }
    // The CSRF cookie is readable by the SPA, still SameSite=Strict.
    const csrfCookie = setCookies(res).find((c) => c.startsWith(`${COOKIES.csrf}=`))!;
    expect(csrfCookie).not.toMatch(/HttpOnly/);
    expect(csrfCookie).toMatch(/; SameSite=Strict/);
  });

  it('marks cookies Secure unless COOKIE_SECURE=false', async () => {
    expect(testConfig({ COOKIE_SECURE: '' }).COOKIE_SECURE).toBe(true);
    const app = express().get('/', (_req, res) => {
      const cookies = cookieWriter(true);
      cookies.setSession(res, { accessToken: 'a', refreshToken: 'r', refreshTokenExpiresAt: '2031-01-01T00:00:00Z' });
      cookies.setCsrf(res);
      res.end();
    });
    const res = await supertest(app).get('/');
    expect(setCookies(res)).toHaveLength(3);
    for (const c of setCookies(res)) expect(c).toMatch(/; Secure/);
  });
});

describe('CSRF', () => {
  it('rejects a non-GET without the header, with a wrong one, and without the cookie', async () => {
    const admin = await ctx.login(ADMIN);
    expectError(await admin.agent.post('/api/session/logout'), 403, 'csrf_invalid');
    expectError(await admin.agent.post('/api/session/logout').set(CSRF_HEADER, 'x' + admin.csrf.slice(1)), 403, 'csrf_invalid');
    expectError(await admin.agent.put(`/api/accounts/${ctx.ids.player}/role`).send({ role: 'gm' }), 403, 'csrf_invalid');
    // Header alone (no cookie, e.g. a cross-site form) is not enough either.
    expectError(await ctx.api().post('/api/session/login').set(CSRF_HEADER, admin.csrf).send({}), 403, 'csrf_invalid');
    // Still signed in, and nothing was changed.
    expect((await admin.get('/api/session')).status).toBe(200);
    expect(await ctx.auditRows()).toEqual([]);
  });
});

describe('session lifetime', () => {
  it('is 401 without cookies', async () => {
    expectError(await ctx.api().get('/api/session'), 401, 'unauthorized');
    expectError(await ctx.api().get('/api/accounts'), 401, 'unauthorized');
  });

  it('refreshes an expired access token through auth, once for parallel requests', async () => {
    const admin = await ctx.login(ADMIN);
    ctx.clock.advance(16 * MINUTE); // access tokens live 15 minutes
    const [a, b] = await Promise.all([admin.get('/api/session'), admin.get('/api/accounts')]);
    expect(a.status, JSON.stringify(a.body)).toBe(200);
    expect(b.status, JSON.stringify(b.body)).toBe(200);
    expect(cookieFrom(a, COOKIES.access)).toBeTruthy();
    // The shared refresh did not trip auth's reuse detection: the session lives on.
    ctx.clock.advance(1 * MINUTE);
    expect((await admin.get('/api/session')).status).toBe(200);
  });

  it('logout ends the session in auth and clears the cookies', async () => {
    const admin = await ctx.login(ADMIN);
    const res = await admin.post('/api/session/logout');
    expect(res.status).toBe(204);
    expect(cookieFrom(res, COOKIES.access)).toBe('');
    expectError(await admin.get('/api/session'), 401, 'unauthorized');
    const [[row]] = await ctx.authPool.query<import('mysql2').RowDataPacket[]>(
      'SELECT COUNT(*) AS live FROM refresh_tokens WHERE account_id = ? AND revoked_at IS NULL',
      [ctx.ids.admin],
    );
    expect(row!.live).toBe(1); // only the session useTestApp opened
  });

  it('a session whose refresh token was revoked must sign in again', async () => {
    const gm = await ctx.login(GM);
    await ctx.authPool.query('UPDATE refresh_tokens SET revoked_at = ? WHERE account_id = ?', [ctx.clock.now(), ctx.ids.gm]);
    ctx.clock.advance(16 * MINUTE);
    const res = await gm.get('/api/session');
    expectError(res, 401, 'unauthorized');
    expect(cookieFrom(res, COOKIES.refresh)).toBe('');
  });
});

describe('role re-check with auth', () => {
  // A fresh app per test, so its /v1/me cache can't hold a token from another test (the clock restarts each test).
  async function signIn(email: string, deps: Partial<AppDeps> = {}) {
    const saved = ctx.app;
    ctx.app = createApp({ ...ctx.deps, ...deps });
    try {
      return await ctx.login(email);
    } finally {
      ctx.app = saved;
    }
  }
  const rename = (gm: Awaited<ReturnType<typeof signIn>>, id: number, name: string) =>
    gm.put(`/api/characters/${id}/name`, { name });

  it('a GM demoted to player loses writes within RECHECK_MS and is signed out', async () => {
    const id = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    const gm = await signIn(GM);
    const admin = await ctx.login(ADMIN);
    expect((await rename(gm, id, 'Strider')).status).toBe(200);
    expect((await admin.put(`/api/accounts/${ctx.ids.gm}/role`, { role: 'player' })).status).toBe(200);
    ctx.clock.advance(RECHECK_MS);
    const res = await rename(gm, id, 'Elessar');
    expectError(res, 403, 'forbidden');
    expect(cookieFrom(res, COOKIES.access)).toBe('');
    expect((await ctx.character(id)).name).toBe('Strider');
    // The token's claims still say gm, but the session state follows auth.
    expectError(await gm.get('/api/session'), 401, 'unauthorized');
  });

  it('GET /api/session reports the current role, not the token claims', async () => {
    const gm = await signIn(GM);
    await ctx.authPool.query("UPDATE accounts SET role = 'admin' WHERE id = ?", [ctx.ids.gm]);
    expect((await gm.get('/api/session')).body.role).toBe('admin');
  });

  it('a banned or deleted GM gets 401 and its cookies cleared', async () => {
    const id = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    const admin = await ctx.login(ADMIN);
    const gm = await signIn(GM);
    expect((await admin.post(`/api/accounts/${ctx.ids.gm}/ban`, { reason: 'Rogue GM' })).status).toBe(201);
    const res = await rename(gm, id, 'Strider');
    expectError(res, 401, 'unauthorized');
    expect(cookieFrom(res, COOKIES.access)).toBe('');

    const other = await ctx.createAccount('gm2@example.com', 'gm');
    const gm2 = await signIn('gm2@example.com');
    expect((await admin.delete(`/api/accounts/${other}`)).status).toBe(200);
    expectError(await rename(gm2, id, 'Strider'), 401, 'unauthorized');
    expect((await ctx.character(id)).name).toBe('Aragorn');
  });

  it('fails writes and the session state closed when auth is unreachable, never trusting the token role', async () => {
    const id = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    const dead = createAuthClient('http://127.0.0.1:1');
    const gm = await signIn(GM, { auth: { ...ctx.deps.auth, me: dead.me } });
    expectError(await rename(gm, id, 'Strider'), 502, 'auth_unavailable');
    expectError(await gm.get('/api/session'), 502, 'auth_unavailable');
    expect((await ctx.character(id)).name).toBe('Aragorn');
    expect((await gm.get('/api/characters?limit=1')).status).toBe(200); // reads still use the token
  });
});

describe('static SPA', () => {
  it('serves files, falls back to index.html for client routes, and keeps /api 404s as JSON', async () => {
    const dir = await mkdtemp(path.join(tmpdir(), 'chthonic-admin-web-'));
    await writeFile(path.join(dir, 'index.html'), '<!doctype html><title>Chthonic Admin</title>');
    await writeFile(path.join(dir, 'app.js'), 'console.log(1)');
    const app = createApp({ ...ctx.deps, config: { ...ctx.deps.config, STATIC_DIR: dir } });
    expect((await supertest(app).get('/app.js')).text).toBe('console.log(1)');
    const page = await supertest(app).get('/accounts/12');
    expect(page.status).toBe(200);
    expect(page.text).toContain('Chthonic Admin');
    expectError(await supertest(app).get('/api/nope').set('Cookie', 'x=1'), 401, 'unauthorized');
  });

  it('answers 404 JSON when the SPA is not built', async () => {
    const app = createApp({ ...ctx.deps, config: { ...ctx.deps.config, STATIC_DIR: path.join(tmpdir(), 'chthonic-no-such-dir') } });
    expectError(await supertest(app).get('/accounts'), 404, 'not_found');
  });
});
