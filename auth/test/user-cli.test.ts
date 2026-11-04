import { decodeJwt } from 'jose';
import { describe, expect, it } from 'vitest';
import { findAccountByEmail } from '../src/repositories/accounts.js';
import { runUserCommand } from '../src/user-cli.js';
import { createAccount, login, PASSWORD, useTestApp } from './helpers.js';

describe('user CLI', () => {
  const ctx = useTestApp();
  const run = (argv: string[], prompted = PASSWORD) =>
    runUserCommand(ctx.pool, argv, async () => prompted, ctx.clock);

  it('creates a verified account with the role that can log in', async () => {
    const out = await run(['create', ' New@Chthonic.Local ', 'gm', '--password', PASSWORD]);
    expect(out).toMatch(/^Created gm account new@chthonic\.local \(id \d+\)$/);
    const account = await findAccountByEmail(ctx.pool, 'new@chthonic.local');
    expect(account?.role).toBe('gm');
    expect(account?.emailVerifiedAt).not.toBeNull();
    const session = await login(ctx, 'new@chthonic.local');
    expect(decodeJwt(session.accessToken).role).toBe('gm');
  });

  it('prompts for the password when --password is omitted', async () => {
    await run(['create', 'prompted@chthonic.local', 'admin'], 'prompted password');
    await login(ctx, 'prompted@chthonic.local', 'prompted password');
  });

  it('refuses a duplicate email', async () => {
    await createAccount(ctx, 'dup@chthonic.local');
    await expect(run(['create', 'dup@chthonic.local', 'admin'])).rejects.toThrow(/already exists; use set-role/);
    expect((await findAccountByEmail(ctx.pool, 'dup@chthonic.local'))?.role).toBe('player');
  });

  it('rejects bad input without touching the database', async () => {
    await expect(run(['create', 'not-an-email', 'gm'])).rejects.toThrow(/email/);
    await expect(run(['create', 'a@chthonic.local', 'god'])).rejects.toThrow(/role/);
    await expect(run(['create', 'a@chthonic.local', 'gm'], 'short')).rejects.toThrow(/password/);
    await expect(run(['create', 'a@chthonic.local'])).rejects.toThrow(/Expected a command/);
    await expect(run(['delete', 'a@chthonic.local', 'gm'])).rejects.toThrow(/Unknown command/);
    await expect(run(['create', 'a@chthonic.local', 'gm', '--bogus'])).rejects.toThrow(/bogus/);
    expect(await findAccountByEmail(ctx.pool, 'a@chthonic.local')).toBeUndefined();
  });

  it('set-role changes the role of an existing account', async () => {
    const id = await createAccount(ctx, 'promote@chthonic.local');
    expect(await run(['set-role', 'Promote@chthonic.local', 'admin'])).toBe(
      `Changed promote@chthonic.local (id ${id}) from player to admin`,
    );
    expect((await findAccountByEmail(ctx.pool, 'promote@chthonic.local'))?.role).toBe('admin');
  });

  it('set-role fails for an unknown email', async () => {
    await expect(run(['set-role', 'ghost@chthonic.local', 'gm'])).rejects.toThrow(/No account with email/);
  });
});
