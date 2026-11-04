import net from 'node:net';
import tls from 'node:tls';
import { afterAll, beforeAll, beforeEach, describe, expect, it, vi } from 'vitest';
import { createWorldAdminClient, encodeFrame } from '../src/worldAdmin.js';
import { CERT, FakeWorld, KEY } from './fakeWorld.js';

const SECRET = 'test-world-admin-secret';
const world = new FakeWorld(SECRET);
const client = createWorldAdminClient({ secret: SECRET, ca: CERT, timeoutMs: 1000 });
const at = () => ({ host: '127.0.0.1', port: world.port });

beforeAll(() => world.start());
afterAll(() => world.stop());
beforeEach(() => world.reset());

const session = (i: number) => ({
  accountId: 2 ** 40 + i,
  charId: i,
  name: `Name${i}é`,
  map: i % 65536,
  sessionId: 4_000_000_000 + i,
  connectedAtMs: 1_900_000_000_000 + i,
});

describe('world admin client', () => {
  it('frames little-endian: AdminKicked{0x01020304} = 04 00 03 03 04 03 02 01', () => {
    const payload = Buffer.alloc(4);
    payload.writeUInt32LE(0x01020304);
    expect(encodeFrame(0x0303, payload).toString('hex')).toBe('0400030304030201');
  });

  it('says LinkHello{10, secret} and reads a multi-frame online list', async () => {
    world.sessions = Array.from({ length: 2500 }, (_, i) => session(i + 1));
    world.chunkSize = 1000;
    const online = await client.online(at());
    expect(world.hellos).toEqual([{ version: 10, secret: SECRET }]);
    expect(online).toHaveLength(2500);
    expect(online[1]).toEqual({
      accountId: 2 ** 40 + 2,
      characterId: 2,
      characterName: 'Name2é',
      mapId: 2,
      sessionId: 4_000_000_002,
      connectedAt: new Date(1_900_000_000_002).toISOString(),
    });
    world.sessions = [];
    expect(await client.online(at())).toEqual([]); // one frame, count 0
  });

  it('reads a multi-frame instance list: world maps (instance 0) and copies, emptySince null while occupied', async () => {
    world.instances = Array.from({ length: 250 }, (_, i) => ({
      map: i === 0 ? 1 : 6,
      mapName: i === 0 ? 'Start' : 'Crypté',
      instance: i === 0 ? 0 : 4_000_000_000 + i,
      key: i % 2 ? `party-${i}` : '',
      players: i % 3,
      npcs: 2 * i,
      createdAtMs: 1_900_000_000_000 + i,
      emptySinceMs: i % 3 ? 0 : 1_900_000_100_000 + i,
    }));
    world.chunkSize = 100;
    const list = await client.instances(at());
    expect(list).toHaveLength(250);
    expect(list[0]).toEqual({
      mapId: 1,
      mapName: 'Start',
      instanceId: 0,
      key: '',
      players: 0,
      npcs: 0,
      createdAt: new Date(1_900_000_000_000).toISOString(),
      emptySince: new Date(1_900_000_100_000).toISOString(),
    });
    expect(list[1]).toEqual({
      mapId: 6,
      mapName: 'Crypté',
      instanceId: 4_000_000_001,
      key: 'party-1',
      players: 1,
      npcs: 2,
      createdAt: new Date(1_900_000_000_001).toISOString(),
      emptySince: null,
    });
    world.instances = [];
    expect(await client.instances(at())).toEqual([]); // one frame, count 0
  });

  it('kicks by account or character with the reason', async () => {
    world.sessions = [session(1), session(2)];
    expect(await client.kick(at(), { characterId: 2 }, 'stuck')).toBe(1);
    expect(await client.kick(at(), { accountId: 2 ** 40 + 1 }, 'ban')).toBe(1);
    expect(world.kicks).toEqual([
      { accountId: 0n, charId: 2n, reason: 'stuck' },
      { accountId: BigInt(2 ** 40 + 1), charId: 0n, reason: 'ban' },
    ]);
  });

  it('answers a Ping with a Pong carrying the same nonce', async () => {
    world.pingFirst = true;
    expect(await client.online(at())).toEqual([]);
    await vi.waitFor(() => expect(world.pongs).toEqual([0x12345678])); // arrives just after the answer
  });

  it('wrong secret, refused connection, untrusted cert and silence are all 503 realm_unreachable', async () => {
    const wrong = createWorldAdminClient({ secret: 'wrong-secret-0123456789', ca: CERT, timeoutMs: 1000 });
    await expect(wrong.online(at())).rejects.toMatchObject({ status: 503, code: 'realm_unreachable' });

    await expect(client.online({ host: '127.0.0.1', port: 1 })).rejects.toMatchObject({ code: 'realm_unreachable' });

    const noCa = createWorldAdminClient({ secret: SECRET, timeoutMs: 1000 });
    await expect(noCa.online(at())).rejects.toMatchObject({ code: 'realm_unreachable' });

    // TCP accepts but never speaks TLS: the timeout fires.
    const silent = net.createServer(() => {});
    await new Promise<void>((resolve) => silent.listen(0, '127.0.0.1', resolve));
    const started = Date.now();
    await expect(client.online({ host: '127.0.0.1', port: (silent.address() as net.AddressInfo).port })).rejects.toMatchObject({
      code: 'realm_unreachable',
    });
    expect(Date.now() - started).toBeLessThan(3000);
    silent.close();
  });

  it('EOF after the TLS handshake instead of LinkOk is 503', async () => {
    const eof = tls.createServer({ key: KEY, cert: CERT }, (s) => s.destroy());
    await new Promise<void>((resolve) => eof.listen(0, '127.0.0.1', resolve));
    await expect(client.online({ host: '127.0.0.1', port: (eof.address() as net.AddressInfo).port })).rejects.toMatchObject({
      code: 'realm_unreachable',
    });
    eof.close();
  });
});
