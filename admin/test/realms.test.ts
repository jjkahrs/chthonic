import { describe, expect, it } from 'vitest';
import { ADMIN, GM, expectError, goOnline, useTestApp } from './helpers.js';

const ctx = useTestApp();

const realm = { name: 'Bravo', host: 'bravo.example', port: 7001, region: 'eu' };

describe('realms', () => {
  it('admins see auth realms with admin endpoints and reachability; GMs only id, name, reachable', async () => {
    const down = await ctx.addRealm('Down', 1);
    const admin = await ctx.login(ADMIN);
    const list = (await admin.get('/api/realms')).body.realms;
    expect(list).toMatchObject([
      { id: ctx.realmId, name: 'Alpha', adminHost: '127.0.0.1', adminPort: ctx.world.port, reachable: true },
      { id: down, name: 'Down', adminPort: 1, reachable: false },
    ]);
    const gm = await ctx.login(GM);
    expect((await gm.get('/api/realms')).body.realms).toEqual([
      { id: ctx.realmId, name: 'Alpha', reachable: true },
      { id: down, name: 'Down', reachable: false },
    ]);
  });

  it('admin CRUD goes to auth, is audited, and keeps the GM endpoint copy current', async () => {
    const admin = await ctx.login(ADMIN);
    const gm = await ctx.login(GM);
    const created = await admin.post('/api/realms', { ...realm, adminHost: '127.0.0.1', adminPort: ctx.world.port });
    expect(created.status, JSON.stringify(created.body)).toBe(201);
    const id = created.body.id;
    expect((await admin.get(`/api/realms/${id}`)).body).toMatchObject({ name: 'Bravo', adminPort: ctx.world.port });
    expect((await gm.get('/api/realms')).body.realms.map((r: { name: string }) => r.name)).toEqual(['Alpha', 'Bravo']);

    const updated = await admin.put(`/api/realms/${id}`, { ...realm, name: 'Charlie' });
    expect(updated.body).toMatchObject({ name: 'Charlie', adminHost: null, adminPort: null });
    expectError(await gm.get(`/api/realms/${id}/online`), 503, 'realm_unreachable');

    expectError(await admin.post('/api/realms', { ...realm, name: 'Alpha' }), 409, 'realm_name_taken');
    expectError(await admin.post('/api/realms', { ...realm, port: 0 }), 400, 'validation_error');
    expect((await admin.delete(`/api/realms/${id}`)).status).toBe(204);
    expectError(await gm.get(`/api/realms/${id}/online`), 503, 'realm_unreachable'); // gone from the GM copy
    expectError(await admin.get(`/api/realms/${id}/online`), 404, 'realm_not_found');

    expect((await ctx.auditRows()).map((r) => `${r.action}:${r.result}:${r.target_id}`)).toEqual([
      `realm.create:ok:${id}`,
      `realm.update:ok:${id}`,
      'realm.create:realm_name_taken:null',
      'realm.create:validation_error:null',
      `realm.delete:ok:${id}`,
    ]);
  });

  it('GM lists who is online and kicks by account or character', async () => {
    goOnline(ctx, ctx.ids.player, 11, 'Aragorn');
    goOnline(ctx, ctx.ids.otherPlayer); // lobby
    const gm = await ctx.login(GM);
    const online = (await gm.get(`/api/realms/${ctx.realmId}/online`)).body.online;
    expect(online).toEqual([
      {
        accountId: ctx.ids.player,
        characterId: 11,
        characterName: 'Aragorn',
        mapId: 1,
        sessionId: 1,
        connectedAt: new Date(1_900_000_000_000).toISOString(),
      },
      expect.objectContaining({ accountId: ctx.ids.otherPlayer, characterId: 0, characterName: '' }),
    ]);

    expect((await gm.post(`/api/realms/${ctx.realmId}/kick`, { characterId: 11, reason: 'stuck' })).body).toEqual({ kicked: 1 });
    expect((await gm.post(`/api/realms/${ctx.realmId}/kick`, { characterId: 11, reason: 'stuck' })).body).toEqual({ kicked: 0 });
    expect((await gm.post(`/api/realms/${ctx.realmId}/kick`, { accountId: ctx.ids.otherPlayer, reason: 'afk' })).body).toEqual({
      kicked: 1,
    });
    expect(ctx.world.kicks.map((k) => k.reason)).toEqual(['stuck', 'stuck', 'afk']);
    for (const bad of [{}, { reason: 'x' }, { accountId: 1, characterId: 2, reason: 'x' }, { accountId: 1, reason: '' }]) {
      expectError(await gm.post(`/api/realms/${ctx.realmId}/kick`, bad), 400, 'validation_error');
    }
    const rows = await ctx.auditRows();
    expect(rows[0]).toMatchObject({ action: 'realm.kick', target_id: String(ctx.realmId), result: 'ok' });
    expect(rows[0]!.after).toEqual({ characterId: 11, reason: 'stuck', kicked: 1 });
  });

  it('GMs and admins list the live map contexts and instance copies; refresh shows the change', async () => {
    ctx.world.instances = [
      { map: 1, mapName: 'Start', instance: 0, key: '', players: 3, npcs: 40, createdAtMs: 1_900_000_000_000, emptySinceMs: 0 },
      { map: 6, mapName: 'Crypt', instance: 77, key: 'party-7', players: 0, npcs: 1, createdAtMs: 1_900_000_001_000, emptySinceMs: 1_900_000_002_000 },
    ];
    const gm = await ctx.login(GM);
    expect((await gm.get(`/api/realms/${ctx.realmId}/instances`)).body.instances).toEqual([
      {
        mapId: 1,
        mapName: 'Start',
        instanceId: 0,
        key: '',
        players: 3,
        npcs: 40,
        createdAt: new Date(1_900_000_000_000).toISOString(),
        emptySince: null,
      },
      {
        mapId: 6,
        mapName: 'Crypt',
        instanceId: 77,
        key: 'party-7',
        players: 0,
        npcs: 1,
        createdAt: new Date(1_900_000_001_000).toISOString(),
        emptySince: new Date(1_900_000_002_000).toISOString(),
      },
    ]);
    ctx.world.instances.pop(); // the copy timed out
    const admin = await ctx.login(ADMIN);
    expect((await admin.get(`/api/realms/${ctx.realmId}/instances`)).body.instances.map((i: { mapId: number }) => i.mapId)).toEqual([1]);
    const down = await ctx.addRealm('Down', 1);
    expectError(await admin.get(`/api/realms/${down}/instances`), 503, 'realm_unreachable');
    expectError(await admin.get('/api/realms/999/instances'), 404, 'realm_not_found');
    expectError(await gm.get('/api/realms/x/instances'), 400, 'validation_error');
  });

  it('online on a down realm is 503 realm_unreachable', async () => {
    const down = await ctx.addRealm('Down', 1);
    const admin = await ctx.login(ADMIN);
    expectError(await admin.get(`/api/realms/${down}/online`), 503, 'realm_unreachable');
    expectError(await admin.post(`/api/realms/${down}/kick`, { accountId: 1, reason: 'x' }), 503, 'realm_unreachable');
    expectError(await admin.get('/api/realms/999/online'), 404, 'realm_not_found');
  });

  it('no response a GM can reach carries a realm admin endpoint', async () => {
    const down = await ctx.addRealm('Down', 1);
    const unknown = 999;
    await ctx.login(ADMIN);
    goOnline(ctx, ctx.ids.player, 11, 'Aragorn');
    const charId = await ctx.addCharacter({ accountId: ctx.ids.player, name: 'Aragorn' });
    const downChar = await ctx.addCharacter({ accountId: ctx.ids.otherPlayer, name: 'Boromir', realmId: down });
    const gm = await ctx.login(GM);
    const responses = [
      await gm.get('/api/session'),
      await gm.get('/api/realms'),
      await gm.get(`/api/realms/${ctx.realmId}/online`),
      await gm.get(`/api/realms/${down}/online`),
      await gm.get(`/api/realms/${unknown}/online`),
      await gm.get(`/api/realms/${ctx.realmId}`),
      await gm.post(`/api/realms/${ctx.realmId}/kick`, { characterId: 11, reason: 'x' }),
      await gm.post(`/api/realms/${down}/kick`, { accountId: 1, reason: 'x' }),
      await gm.get('/api/characters'),
      await gm.get(`/api/characters/${charId}`),
      await gm.put(`/api/characters/${charId}/name`, { name: 'Strider' }),
      await gm.put(`/api/characters/${downChar}/name`, { name: 'Faramir' }),
      await gm.put(`/api/characters/${downChar}/name`, { name: 'Faramir', force: true }),
      await gm.post(`/api/accounts/${ctx.ids.player}/ban`, { reason: 'x' }),
      await gm.get(`/api/accounts/${ctx.ids.player}`),
      await gm.get('/api/accounts'),
      await gm.get('/api/audit'),
      await gm.post('/api/realms', { name: 'X', host: 'h', port: 1, region: 'r', adminHost: '127.0.0.1', adminPort: 9 }),
    ];
    for (const res of responses) {
      const text = JSON.stringify(res.body) + JSON.stringify(res.headers);
      expect(text, `response ${responses.indexOf(res)}`).not.toMatch(/adminHost|adminPort|admin_host|admin_port|127\.0\.0\.1/);
      expect(text, `response ${responses.indexOf(res)}`).not.toContain(`:${ctx.world.port}`);
    }
  });
});
