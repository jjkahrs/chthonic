import type { RowDataPacket } from 'mysql2/promise';
import { describe, expect, it } from 'vitest';
import { HOUR, MINUTE, expectError, useTestApp } from './helpers.js';

const ctx = useTestApp();

const endpoints = [
  { path: '/v1/auth/login', limit: 20, window: MINUTE, body: { email: 'x@example.com', password: 'whatever' } },
  { path: '/v1/auth/register', limit: 5, window: HOUR, body: { email: 'x@example.com', password: 'long enough pw' } },
  { path: '/v1/auth/forgot-password', limit: 5, window: HOUR, body: { email: 'x@example.com' } },
  { path: '/v1/auth/resend-verification', limit: 5, window: HOUR, body: { email: 'x@example.com' } },
];

async function exhaust(path: string, limit: number, body: object): Promise<void> {
  for (let i = 0; i < limit; i++) {
    const res = await ctx.api().post(path).send(body);
    expect(res.status).not.toBe(429);
  }
}

describe.each(endpoints)('rate limit on $path', ({ path, limit, window, body }) => {
  it(`allows ${limit} requests, then returns 429 with Retry-After until the window ends`, async () => {
    await exhaust(path, limit, body);
    const blocked = await ctx.api().post(path).send(body);
    expectError(blocked, 429, 'rate_limited');
    expect(Number(blocked.headers['retry-after'])).toBe(window / 1000);

    ctx.clock.advance(window - 1000);
    const stillBlocked = await ctx.api().post(path).send(body);
    expectError(stillBlocked, 429, 'rate_limited');
    expect(stillBlocked.headers['retry-after']).toBe('1');

    ctx.clock.advance(1000);
    expect((await ctx.api().post(path).send(body)).status).not.toBe(429);
  });
});

describe('rate limiter storage', () => {
  it('keeps limits across an app restart (state lives in MySQL)', async () => {
    const { path, limit, body } = endpoints[0]!;
    await exhaust(path, limit, body);
    ctx.restart();
    expectError(await ctx.api().post(path).send(body), 429, 'rate_limited');
  });

  it('counts per IP when behind a trusted proxy', async () => {
    ctx.restart({ config: { ...ctx.config, TRUST_PROXY: 'true' } });
    const { path, limit, body } = endpoints[1]!;
    for (let i = 0; i < limit; i++) await ctx.api().post(path).set('X-Forwarded-For', '203.0.113.1').send(body);
    expectError(await ctx.api().post(path).set('X-Forwarded-For', '203.0.113.1').send(body), 429, 'rate_limited');
    expect((await ctx.api().post(path).set('X-Forwarded-For', '203.0.113.2').send(body)).status).toBe(202);
  });

  it('prunes expired windows on write', async () => {
    const { path, body } = endpoints[0]!;
    await ctx.api().post(path).send(body);
    ctx.clock.advance(HOUR);
    await ctx.api().post(path).send(body);
    const [rows] = await ctx.pool.query<RowDataPacket[]>('SELECT window_start FROM rate_limits');
    expect(rows).toHaveLength(1);
  });
});
