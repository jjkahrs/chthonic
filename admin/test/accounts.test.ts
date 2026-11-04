import { describe, expect, it } from 'vitest';
import { ADMIN, GM, OTHER, PLAYER, expectError, goOnline, useTestApp } from './helpers.js';

const ctx = useTestApp();

describe('read', () => {
  it('admin and gm list and view accounts through auth; GMs only see players', async () => {
    const admin = await ctx.login(ADMIN);
    const gm = await ctx.login(GM);
    const all = (await admin.get('/api/accounts')).body;
    expect(all.total).toBe(4);
    const players = (await gm.get('/api/accounts')).body;
    expect(players.accounts.map((a: { email: string }) => a.email)).toEqual([PLAYER, OTHER]);
    expect((await admin.get('/api/accounts?q=other&limit=10')).body.accounts.map((a: { id: number }) => a.id)).toEqual([
      ctx.ids.otherPlayer,
    ]);
    const detail = await gm.get(`/api/accounts/${ctx.ids.player}`);
    expect(detail.body).toMatchObject({ id: ctx.ids.player, email: PLAYER, role: 'player', bans: [] });
    // Auth enforces GM limits; its codes come through unchanged.
    expectError(await gm.get(`/api/accounts/${ctx.ids.admin}`), 403, 'forbidden');
    expectError(await admin.get('/api/accounts/999'), 404, 'account_not_found');
    expectError(await admin.get('/api/accounts?limit=0'), 400, 'validation_error');
  });
});

describe('create', () => {
  it('creates a verified account and audits it without the password', async () => {
    const admin = await ctx.login(ADMIN);
    const res = await admin.post('/api/accounts', { email: 'New@Example.com', password: 'a long password', role: 'gm' });
    expect(res.status, JSON.stringify(res.body)).toBe(201);
    expect(res.body).toMatchObject({ email: 'new@example.com', role: 'gm' });
    expect(res.body.emailVerifiedAt).not.toBeNull();
    expectError(await admin.post('/api/accounts', { email: 'new@example.com', password: 'a long password' }), 409, 'email_taken');
    expectError(await admin.post('/api/accounts', { email: 'x@example.com', password: 'short' }), 400, 'validation_error');

    const [ok, dup] = await ctx.auditRows();
    expect(ok).toMatchObject({ action: 'account.create', result: 'ok', target_id: String(res.body.id) });
    expect(ok!.after).toEqual({ email: 'new@example.com', role: 'gm', id: res.body.id });
    expect(JSON.stringify(ok!.after)).not.toContain('a long password');
    expect(dup).toMatchObject({ action: 'account.create', result: 'email_taken' });
  });
});

describe('delete', () => {
  it('kicks on every realm, deletes in auth, then soft-deletes every character', async () => {
    const second = await ctx.addRealm('Bravo', ctx.world.port); // same fake world, a second realm
    const a = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    const b = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Arwen', realmId: second });
    const keep = await ctx.addCharacter({ accountId: ctx.ids.otherPlayer, name: 'Legolas' });
    goOnline(ctx, ctx.ids.player, a, 'Aragorn');
    const admin = await ctx.login(ADMIN);

    const res = await admin.delete(`/api/accounts/${ctx.ids.player}`);
    expect(res.status, JSON.stringify(res.body)).toBe(200);
    expect(res.body).toEqual({ kicks: { kicked: 1, failures: [] }, charactersDeleted: 2 });
    expect(ctx.world.kicks.map((k) => k.accountId)).toEqual([BigInt(ctx.ids.player), BigInt(ctx.ids.player)]);
    expectError(await admin.get(`/api/accounts/${ctx.ids.player}`), 404, 'account_not_found');
    expect((await ctx.character(a)).deleted_at).not.toBeNull();
    expect((await ctx.character(b)).deleted_at).not.toBeNull();
    expect((await ctx.character(keep)).deleted_at).toBeNull();

    const [row] = await ctx.auditRows();
    expect(row).toMatchObject({ action: 'account.delete', result: 'ok', target_id: String(ctx.ids.player) });
    expect(row!.before.email).toBe(PLAYER);
  });

  it('refuses self-delete and unknown accounts before kicking anyone', async () => {
    const admin = await ctx.login(ADMIN);
    expectError(await admin.delete(`/api/accounts/${ctx.ids.admin}`), 400, 'cannot_modify_self');
    expectError(await admin.delete('/api/accounts/999'), 404, 'account_not_found');
    expect(ctx.world.kicks).toEqual([]);
    expect((await ctx.auditRows()).map((r) => r.result)).toEqual(['cannot_modify_self', 'account_not_found']);
  });

  it('an unreachable realm is reported, not blocking', async () => {
    const down = await ctx.addRealm('Down', 1);
    const admin = await ctx.login(ADMIN);
    const res = await admin.delete(`/api/accounts/${ctx.ids.player}`);
    expect(res.status).toBe(200);
    expect(res.body.kicks).toEqual({ kicked: 0, failures: [{ realmId: down, code: 'realm_unreachable' }] });
  });
});

describe('ban', () => {
  it('a GM ban kicks the player everywhere and unban lifts it', async () => {
    const down = await ctx.addRealm('Down', 1);
    await ctx.login(ADMIN);
    goOnline(ctx, ctx.ids.player, 5, 'Aragorn');
    goOnline(ctx, ctx.ids.otherPlayer, 6, 'Legolas');
    const gm = await ctx.login(GM);

    const res = await gm.post(`/api/accounts/${ctx.ids.player}/ban`, { reason: 'Gold botting' });
    expect(res.status, JSON.stringify(res.body)).toBe(201);
    expect(res.body.ban).toMatchObject({ accountId: ctx.ids.player, reason: 'Gold botting', bannedBy: ctx.ids.gm });
    expect(res.body.kicks).toEqual({ kicked: 1, failures: [{ realmId: down, code: 'realm_unreachable' }] });
    expect(ctx.world.sessions.map((s) => s.accountId)).toEqual([ctx.ids.otherPlayer]);
    expect((await gm.get(`/api/accounts/${ctx.ids.player}`)).body.activeBan.reason).toBe('Gold botting');

    expect((await gm.delete(`/api/accounts/${ctx.ids.player}/ban`)).status).toBe(204);
    expect((await gm.get(`/api/accounts/${ctx.ids.player}`)).body.activeBan).toBeNull();

    const rows = await ctx.auditRows();
    expect(rows.map((r) => [r.action, r.result, r.actor_role])).toEqual([
      ['account.ban', 'ok', 'gm'],
      ['account.unban', 'ok', 'gm'],
    ]);
    expect(rows[0]!.after.kicks.failures).toEqual([{ realmId: down, code: 'realm_unreachable' }]);
  });

  it('GMs cannot ban staff; nothing is kicked', async () => {
    const gm = await ctx.login(GM);
    expectError(await gm.post(`/api/accounts/${ctx.ids.admin}/ban`, { reason: 'x' }), 403, 'forbidden');
    expect(ctx.world.kicks).toEqual([]);
  });
});

describe('role', () => {
  it('a role change kicks the account; setting the same role does not', async () => {
    const admin = await ctx.login(ADMIN);
    goOnline(ctx, ctx.ids.gm, 7, 'Gandalf');
    const same = await admin.put(`/api/accounts/${ctx.ids.gm}/role`, { role: 'gm' });
    expect(same.body).toMatchObject({ account: { role: 'gm' }, kicks: null });
    expect(ctx.world.kicks).toEqual([]);

    const demoted = await admin.put(`/api/accounts/${ctx.ids.gm}/role`, { role: 'player' });
    expect(demoted.status).toBe(200);
    expect(demoted.body).toMatchObject({ account: { role: 'player' }, kicks: { kicked: 1, failures: [] } });
    expect(ctx.world.sessions).toEqual([]);
    const rows = await ctx.auditRows();
    expect(rows[1]).toMatchObject({ action: 'account.set_role', result: 'ok' });
    expect(rows[1]!.before).toEqual({ role: 'gm' });
    expectError(await admin.put(`/api/accounts/${ctx.ids.admin}/role`, { role: 'gm' }), 400, 'cannot_modify_self');
  });
});

describe('account actions', () => {
  it('verify-email, password-reset, unlock and revoke-sessions go to auth', async () => {
    const admin = await ctx.login(ADMIN);
    await ctx.authPool.query('UPDATE accounts SET email_verified_at = NULL, failed_login_count = 3 WHERE id = ?', [
      ctx.ids.player,
    ]);
    for (const action of ['verify-email', 'password-reset', 'unlock', 'revoke-sessions']) {
      expect((await admin.post(`/api/accounts/${ctx.ids.player}/${action}`)).status).toBe(204);
      expectError(await admin.post(`/api/accounts/999/${action}`), 404, 'account_not_found');
    }
    const detail = (await admin.get(`/api/accounts/${ctx.ids.player}`)).body;
    expect(detail.emailVerifiedAt).not.toBeNull();
    expect(detail.failedLoginCount).toBe(0);
    expect((await ctx.auditRows()).map((r) => `${r.action}:${r.result}`)).toEqual([
      'account.verify_email:ok',
      'account.verify_email:account_not_found',
      'account.password_reset:ok',
      'account.password_reset:account_not_found',
      'account.unlock:ok',
      'account.unlock:account_not_found',
      'account.revoke_sessions:ok',
      'account.revoke_sessions:account_not_found',
    ]);
  });
});
