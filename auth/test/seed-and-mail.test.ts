import type { RowDataPacket } from 'mysql2/promise';
import { describe, expect, it } from 'vitest';
import { silentLogger } from '../src/logger.js';
import { createSmtpMailer } from '../src/mailer.js';
import { seedAdmin } from '../src/services/seed.js';
import { expectError, login, testConfig, useTestApp } from './helpers.js';

const ctx = useTestApp();

describe('seed admin', () => {
  const config = testConfig({ SEED_ADMIN_EMAIL: 'Root@Chthonic.local', SEED_ADMIN_PASSWORD: 'seed admin password' });

  it('creates one pre-verified admin and never duplicates or overwrites it', async () => {
    await seedAdmin(ctx.pool, config, ctx.clock, silentLogger);
    await seedAdmin(ctx.pool, config, ctx.clock, silentLogger);
    await seedAdmin(ctx.pool, { ...config, SEED_ADMIN_PASSWORD: 'a different password' }, ctx.clock, silentLogger);

    const [rows] = await ctx.pool.query<RowDataPacket[]>('SELECT email, role, email_verified_at FROM accounts');
    expect(rows).toHaveLength(1);
    expect(rows[0]).toMatchObject({ email: 'root@chthonic.local', role: 'admin' });
    expect(rows[0]?.email_verified_at).not.toBeNull();
    const session = await login(ctx, 'root@chthonic.local', 'seed admin password');
    expect((await ctx.api().get('/v1/admin/realms').set({ Authorization: `Bearer ${session.accessToken}` })).status).toBe(200);
  });

  it('does nothing when the seed vars are unset', async () => {
    await seedAdmin(ctx.pool, testConfig(), ctx.clock, silentLogger);
    const [rows] = await ctx.pool.query<RowDataPacket[]>('SELECT COUNT(*) AS n FROM accounts');
    expect(rows[0]?.n).toBe(0);
  });
});

describe('config', () => {
  it('fails fast with every problem listed', () => {
    expect(() => testConfig({ DB_HOST: '', SMTP_PORT: 'abc', SEED_ADMIN_PASSWORD: 'short pw' })).toThrow(
      /Invalid configuration:[\s\S]*DB_HOST[\s\S]*SMTP_PORT[\s\S]*SEED_ADMIN_PASSWORD/,
    );
  });

  it('refuses the .env.example placeholder seed password, in any case', () => {
    const email = 'admin@chthonic.local';
    for (const pw of ['change-me-admin-password', ' CHANGE-ME-xyz12345 ']) {
      expect(() => testConfig({ SEED_ADMIN_EMAIL: email, SEED_ADMIN_PASSWORD: pw })).toThrow(
        /Invalid configuration:[\s\S]*SEED_ADMIN_PASSWORD: is the \.env\.example placeholder/,
      );
    }
    expect(testConfig({ SEED_ADMIN_EMAIL: email, SEED_ADMIN_PASSWORD: 'a real admin password' }).SEED_ADMIN_PASSWORD).toBe(
      'a real admin password',
    );
    expect(testConfig({ SEED_ADMIN_EMAIL: '', SEED_ADMIN_PASSWORD: '' }).SEED_ADMIN_PASSWORD).toBeUndefined();
  });
});

interface MailpitList {
  messages: Array<{ ID: string; Subject: string; To: Array<{ Address: string }> }>;
}

describe('SMTP mailer → Mailpit', () => {
  it('delivers a real verification email readable through the Mailpit API', async () => {
    const mailpit = process.env.MAILPIT_API_URL ?? 'http://127.0.0.1:8025';
    const to = `mailpit-${Date.now()}@example.com`;
    ctx.restart({ mailer: createSmtpMailer(ctx.config) });

    expect((await ctx.api().post('/v1/auth/register').send({ email: to, password: 'mailpit password' })).status).toBe(202);

    let message: MailpitList['messages'][number] | undefined;
    for (let i = 0; i < 50 && !message; i++) {
      const list = (await (await fetch(`${mailpit}/api/v1/search?query=${encodeURIComponent(`to:${to}`)}`)).json()) as MailpitList;
      message = list.messages[0];
      if (!message) await new Promise((r) => setTimeout(r, 100));
    }
    expect(message?.Subject).toBe('Verify your Chthonic account');

    const body = (await (await fetch(`${mailpit}/api/v1/message/${message!.ID}`)).json()) as { Text: string };
    const token = body.Text.match(/token=([\w-]+)/)?.[1];
    expect((await ctx.api().post('/v1/auth/verify-email').send({ token })).status).toBe(200);
    expectError(await ctx.api().post('/v1/auth/verify-email').send({ token }), 400, 'invalid_token');
  });
});
