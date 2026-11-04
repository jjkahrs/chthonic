import { describe, expect, it } from 'vitest';
import { ADMIN, GM, expectError, goOnline, useTestApp, type Client } from './helpers.js';

const ctx = useTestApp();

describe('list and detail', () => {
  it('filters by realm, account, name prefix and deleted; paginates', async () => {
    const other = await ctx.addRealm('Bravo', null);
    const a = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    const b = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Arwen', deleted: true });
    const c = await ctx.addCharacter({ accountId: ctx.ids.otherPlayer, name: 'Boromir', realmId: other });
    const gm = await ctx.login(GM);
    const ids = async (query: string) => (await gm.get(`/api/characters${query}`)).body.characters.map((x: { id: number }) => x.id);

    expect(await ids('')).toEqual([a, c]);
    expect(await ids('?includeDeleted=true')).toEqual([a, b, c]);
    expect(await ids(`?realmId=${other}`)).toEqual([c]);
    expect(await ids(`?accountId=${ctx.ids.player}&includeDeleted=true`)).toEqual([a, b]);
    expect(await ids('?name=ar&includeDeleted=true')).toEqual([a, b]);
    const page = (await gm.get('/api/characters?limit=1&offset=1')).body;
    expect(page).toMatchObject({ total: 2, limit: 1, offset: 1 });
    expect(page.characters.map((x: { id: number }) => x.id)).toEqual([c]);
    expectError(await gm.get('/api/characters?name=a%25'), 400, 'validation_error');
    expectError(await gm.get('/api/characters?limit=101'), 400, 'validation_error');
  });

  it('detail has every column with components and options as JSON', async () => {
    const id = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    const res = await (await ctx.login(GM)).get(`/api/characters/${id}`);
    expect(res.body).toMatchObject({
      id,
      realmId: ctx.realmId,
      accountId: ctx.ids.player,
      name: 'Aragorn',
      mapId: 0,
      position: { x: 1, y: 2, z: 3 },
      heading: 0.5,
      components: { Hp: { hp: 10 } },
      options: { class: 'warrior' },
      deletedAt: null,
      lastLoginAt: null,
    });
    expectError(await (await ctx.login(GM)).get('/api/characters/999'), 404, 'character_not_found');
  });
});

describe('rename', () => {
  let gm: Client;
  const rename = (id: number, body: object) => gm.put(`/api/characters/${id}/name`, body);

  it('renames, enforcing 3-16 ASCII letters', async () => {
    gm = await ctx.login(GM);
    const id = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    for (const name of ['Ab', 'A'.repeat(17), 'Ara gorn', 'Arag0rn', 'Ärwen', '', 42]) {
      expectError(await rename(id, { name }), 400, 'validation_error');
    }
    const res = await rename(id, { name: 'Strider' });
    expect(res.status, JSON.stringify(res.body)).toBe(200);
    expect(res.body.name).toBe('Strider');
    expect((await ctx.character(id)).name).toBe('Strider');
    expect((await rename(id, { name: 'A'.repeat(16) })).status).toBe(200);
    expect((await rename(id, { name: 'Abc' })).status).toBe(200);
  });

  it('409 name_taken per realm, including deleted characters and any letter case', async () => {
    gm = await ctx.login(GM);
    const id = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    await ctx.addCharacter({ accountId: ctx.ids.otherPlayer, name: 'Legolas' });
    await ctx.addCharacter({ accountId: ctx.ids.otherPlayer, name: 'Gimli', deleted: true });
    expectError(await rename(id, { name: 'Legolas' }), 409, 'name_taken');
    expectError(await rename(id, { name: 'LEGOLAS' }), 409, 'name_taken');
    expectError(await rename(id, { name: 'Gimli' }), 409, 'name_taken');
    // Another realm's names don't count.
    const other = await ctx.addRealm('Bravo', null);
    await ctx.addCharacter({ accountId: ctx.ids.otherPlayer, name: 'Faramir', realmId: other });
    expect((await rename(id, { name: 'Faramir' })).status).toBe(200);
  });
});

describe('online guard', () => {
  it('409 character_online while the account is in the world or the lobby; offline after a kick', async () => {
    const gm = await ctx.login(GM);
    const id = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    goOnline(ctx, ctx.ids.player, id, 'Aragorn');
    const err = expectError(await gm.put(`/api/characters/${id}/name`, { name: 'Strider' }), 409, 'character_online');
    expect(err.details).toEqual({ realmId: ctx.realmId, accountId: ctx.ids.player });
    expect((await ctx.character(id)).name).toBe('Aragorn');

    expect((await gm.post(`/api/realms/${ctx.realmId}/kick`, { characterId: id, reason: 'rename' })).body).toEqual({ kicked: 1 });
    goOnline(ctx, ctx.ids.player); // back in the lobby
    expectError(await gm.delete(`/api/characters/${id}`), 409, 'character_online');
    await gm.post(`/api/realms/${ctx.realmId}/kick`, { accountId: ctx.ids.player, reason: 'rename' });
    expect((await gm.put(`/api/characters/${id}/name`, { name: 'Strider' })).status).toBe(200);
  });

  it('another account being online does not block', async () => {
    const id = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    goOnline(ctx, ctx.ids.otherPlayer, 99, 'Legolas');
    expect((await (await ctx.login(GM)).put(`/api/characters/${id}/name`, { name: 'Strider' })).status).toBe(200);
  });

  it('503 realm_unreachable when the world admin port is down or not configured', async () => {
    const gm = await ctx.login(GM);
    const down = await ctx.addRealm('Down', 1); // nothing listens on port 1
    const unset = await ctx.addRealm('Unset', null);
    await ctx.login(ADMIN); // GM requests use the endpoint copy refreshed by admin reads
    for (const realmId of [down, unset]) {
      const id = await ctx.addCharacter({ accountId: ctx.ids.player, name: `Char${realmId === down ? 'a' : 'b'}`, realmId });
      expectError(await gm.put(`/api/characters/${id}/name`, { name: 'Strider' }), 503, 'realm_unreachable');
    }
  });

  it('503 when the world rejects the secret', async () => {
    const id = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    const real = ctx.world.secret;
    ctx.world.secret = 'some-other-secret-value';
    try {
      expectError(await (await ctx.login(GM)).put(`/api/characters/${id}/name`, { name: 'Strider' }), 503, 'realm_unreachable');
    } finally {
      ctx.world.secret = real;
    }
  });

  it('admin force writes through an unreachable realm and is audited forced=true; GM force is 403', async () => {
    const down = await ctx.addRealm('Down', 1);
    const admin = await ctx.login(ADMIN);
    const gm = await ctx.login(GM);
    const id = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn', realmId: down });

    expectError(await gm.put(`/api/characters/${id}/name`, { name: 'Strider', force: true }), 403, 'forbidden');
    expectError(await admin.put(`/api/characters/${id}/name`, { name: 'Strider' }), 503, 'realm_unreachable');
    const res = await admin.put(`/api/characters/${id}/name`, { name: 'Strider', force: true });
    expect(res.status, JSON.stringify(res.body)).toBe(200);
    expect((await ctx.character(id)).name).toBe('Strider');

    const rows = await ctx.auditRows();
    expect(rows.map((r) => [r.actor_role, r.action, r.result, r.forced])).toEqual([
      ['gm', 'character.rename', 'forbidden', 0],
      ['admin', 'character.rename', 'realm_unreachable', 0],
      ['admin', 'character.rename', 'ok', 1],
    ]);
    expect(rows[2]).toMatchObject({ target_type: 'character', target_id: String(id) });
    expect(rows[2]!.before.name).toBe('Aragorn');
    expect(rows[2]!.after.name).toBe('Strider');
  });

  it('force does not override an online character', async () => {
    const id = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    goOnline(ctx, ctx.ids.player, id, 'Aragorn');
    const admin = await ctx.login(ADMIN);
    expectError(await admin.put(`/api/characters/${id}/name`, { name: 'Strider', force: true }), 409, 'character_online');
  });
});

describe('other writes', () => {
  it('soft-deletes and restores', async () => {
    const gm = await ctx.login(GM);
    const id = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    const del = await gm.delete(`/api/characters/${id}`);
    expect(del.status).toBe(200);
    expect(del.body.deletedAt).toBe(ctx.clock.now().toISOString());
    expect((await ctx.character(id)).deleted_at).not.toBeNull();
    const restored = await gm.post(`/api/characters/${id}/restore`);
    expect(restored.body.deletedAt).toBeNull();
    expect((await ctx.character(id)).deleted_at).toBeNull();
  });

  it('moves to a map and position', async () => {
    const gm = await ctx.login(GM);
    const id = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    const res = await gm.put(`/api/characters/${id}/position`, { mapId: 7, x: -5.5, y: 0, z: 12, heading: 3.14 });
    expect(res.status, JSON.stringify(res.body)).toBe(200);
    expect(res.body).toMatchObject({ mapId: 7, position: { x: -5.5, y: 0, z: 12 }, heading: 3.14 });
    for (const bad of [{ mapId: -1 }, { mapId: 70000 }, { x: 'a' }, { heading: null }]) {
      expectError(
        await gm.put(`/api/characters/${id}/position`, { mapId: 1, x: 0, y: 0, z: 0, heading: 0, ...bad }),
        400,
        'validation_error',
      );
    }
  });

  it('edits components and options as JSON objects with a size limit', async () => {
    const gm = await ctx.login(GM);
    const id = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    const res = await gm.put(`/api/characters/${id}/components`, { components: { Hp: { hp: 99 }, Bag: [1, 2] } });
    expect(res.body.components).toEqual({ Hp: { hp: 99 }, Bag: [1, 2] });
    expect((await gm.put(`/api/characters/${id}/options`, { options: {} })).body.options).toEqual({});
    for (const components of [[1], 'x', null, 5]) {
      expectError(await gm.put(`/api/characters/${id}/components`, { components }), 400, 'validation_error');
    }
    const big = { blob: 'x'.repeat(65 * 1024) };
    expectError(await gm.put(`/api/characters/${id}/options`, { options: big }), 400, 'validation_error');
    expectError(await gm.put(`/api/characters/${id}/options`, { options: { blob: 'x'.repeat(200 * 1024) } }), 413, 'payload_too_large');
  });
});
