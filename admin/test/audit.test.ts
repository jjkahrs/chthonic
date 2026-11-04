import { describe, expect, it } from 'vitest';
import { ADMIN, GM, MINUTE, expectError, useTestApp } from './helpers.js';

const ctx = useTestApp();

describe('audit', () => {
  it('records one row per mutating request with the actor, success or failure', async () => {
    const gm = await ctx.login(GM);
    const id = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    await gm.put(`/api/characters/${id}/name`, { name: 'Strider' });
    await gm.put(`/api/characters/${id}/name`, { name: 'X' });
    await gm.post('/api/accounts', { email: 'n@example.com', password: 'a long password' }); // admin only
    await gm.get('/api/characters'); // reads are not audited

    const rows = await ctx.auditRows();
    expect(rows.map((r) => [r.actor_account_id, r.actor_email, r.actor_role, r.action, r.target_type, r.result])).toEqual([
      [ctx.ids.gm, GM, 'gm', 'character.rename', 'character', 'ok'],
      [ctx.ids.gm, GM, 'gm', 'character.rename', 'character', 'validation_error'],
      [ctx.ids.gm, GM, 'gm', 'account.create', 'account', 'forbidden'],
    ]);
    expect(rows[0]!.created_at).toEqual(ctx.clock.now());
  });

  it('GET /api/audit is admin-only, newest first, with filters and pagination', async () => {
    const admin = await ctx.login(ADMIN);
    const gm = await ctx.login(GM);
    const id = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    await gm.put(`/api/characters/${id}/name`, { name: 'Strider' });
    ctx.clock.advance(10 * MINUTE);
    const middle = ctx.clock.now().toISOString();
    await admin.post(`/api/accounts/${ctx.ids.player}/unlock`);
    ctx.clock.advance(10 * MINUTE);
    await admin.delete(`/api/characters/${id}`);

    expectError(await gm.get('/api/audit'), 403, 'forbidden');
    const all = (await admin.get('/api/audit')).body;
    expect(all.total).toBe(3);
    expect(all.entries.map((e: { action: string }) => e.action)).toEqual([
      'character.delete',
      'account.unlock',
      'character.rename',
    ]);
    expect(all.entries[2]).toMatchObject({
      actorAccountId: ctx.ids.gm,
      actorEmail: GM,
      actorRole: 'gm',
      targetType: 'character',
      targetId: String(id),
      forced: false,
      result: 'ok',
      before: expect.objectContaining({ name: 'Aragorn' }),
      after: expect.objectContaining({ name: 'Strider' }),
    });

    const actions = async (q: string) =>
      (await admin.get(`/api/audit?${q}`)).body.entries.map((e: { action: string }) => e.action);
    expect(await actions(`actorId=${ctx.ids.admin}`)).toEqual(['character.delete', 'account.unlock']);
    expect(await actions('action=account.unlock')).toEqual(['account.unlock']);
    expect(await actions(`targetType=character&targetId=${id}`)).toEqual(['character.delete', 'character.rename']);
    expect(await actions(`from=${middle}`)).toEqual(['character.delete', 'account.unlock']);
    expect(await actions(`to=${middle}`)).toEqual(['character.rename']);
    expect(await actions('limit=1&offset=1')).toEqual(['account.unlock']);
    expectError(await admin.get('/api/audit?from=yesterday'), 400, 'validation_error');
  });
});
