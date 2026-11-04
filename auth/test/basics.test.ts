import { describe, expect, it } from 'vitest';
import { migrate } from '../src/db/migrate.js';
import { createPool } from '../src/db/pool.js';
import { createApp } from '../src/http/app.js';
import { silentLogger } from '../src/logger.js';
import { expectError, testConfig, useTestApp } from './helpers.js';
import supertest from 'supertest';

const ctx = useTestApp();

describe('health and JWKS', () => {
  it('GET /health reports the DB is ok', async () => {
    const res = await ctx.api().get('/health');
    expect(res.status).toBe(200);
    expect(res.body).toEqual({ status: 'ok', db: 'ok' });
  });

  it('GET /health returns 503 db_unavailable when MySQL is unreachable', async () => {
    const config = testConfig({ DB_PORT: '1' });
    const deadPool = createPool(config);
    const app = createApp({ ...ctx, config, pool: deadPool, log: silentLogger });
    expectError(await supertest(app).get('/health'), 503, 'db_unavailable');
    await deadPool.end();
  });

  it('publishes only the public RS256 key', async () => {
    const res = await ctx.api().get('/.well-known/jwks.json');
    expect(res.status).toBe(200);
    expect(res.body.keys).toHaveLength(1);
    const [jwk] = res.body.keys;
    expect(jwk).toMatchObject({ kty: 'RSA', alg: 'RS256', use: 'sig', kid: ctx.keys.kid });
    expect(jwk).toHaveProperty('n');
    expect(jwk).not.toHaveProperty('d');
  });
});

describe('error conventions', () => {
  it('unknown routes return 404 not_found', async () => {
    expectError(await ctx.api().get('/v1/nope'), 404, 'not_found');
  });

  it('malformed JSON returns 400 validation_error', async () => {
    const res = await ctx.api().post('/v1/auth/login').set('Content-Type', 'application/json').send('{"email":');
    expectError(res, 400, 'validation_error');
  });

  it('a missing body returns 400 validation_error with details', async () => {
    const error = expectError(await ctx.api().post('/v1/auth/login'), 400, 'validation_error');
    expect(error.details).toEqual(expect.arrayContaining([expect.objectContaining({ path: expect.any(String) })]));
  });

  it('bodies over the size limit return 413 payload_too_large', async () => {
    const res = await ctx.api().post('/v1/auth/login').send({ email: 'a@b.co', password: 'x'.repeat(20_000) });
    expectError(res, 413, 'payload_too_large');
  });

  it('sets helmet security headers and no-store on API responses', async () => {
    const res = await ctx.api().post('/v1/auth/login').send({});
    expect(res.headers['x-content-type-options']).toBe('nosniff');
    expect(res.headers['x-powered-by']).toBeUndefined();
    expect(res.headers['cache-control']).toBe('no-store');
  });
});

describe('migrations', () => {
  it('are idempotent: a second run applies nothing', async () => {
    expect(await migrate(ctx.config, undefined, () => {})).toEqual([]);
  });
});
