import { readFileSync } from 'node:fs';
import type { AddressInfo } from 'node:net';
import tls from 'node:tls';

// An in-process world admin port written straight from docs/world/protocol.html#admin, independent of
// src/worldAdmin.ts so the client is checked against the wire format, not against itself.

export const CERT = readFileSync(new URL('./fixtures/world-test.crt', import.meta.url));
export const KEY = readFileSync(new URL('./fixtures/world-test.key', import.meta.url));

export interface FakeInstance {
  map: number;
  mapName: string;
  instance: number;
  key: string;
  players: number;
  npcs: number;
  createdAtMs: number;
  emptySinceMs: number;
}

export interface FakeSession {
  accountId: number;
  charId: number;
  name: string;
  map: number;
  sessionId: number;
  connectedAtMs: number;
}

const frame = (op: number, payload: Buffer = Buffer.alloc(0)) => {
  const h = Buffer.alloc(4);
  h.writeUInt16LE(payload.length, 0);
  h.writeUInt16LE(op, 2);
  return Buffer.concat([h, payload]);
};
const disconnect = (reason: number) => frame(0x0103, Buffer.from([reason, 0]));

export class FakeWorld {
  sessions: FakeSession[] = [];
  instances: FakeInstance[] = [];
  kicks: { accountId: bigint; charId: bigint; reason: string }[] = [];
  hellos: { version: number; secret: string }[] = [];
  /** Entries per AdminOnline frame; small values exercise the more=1 chunking. */
  chunkSize = 1000;
  /** Sends a Ping before every answer, which the client must Pong. */
  pingFirst = false;
  pongs: number[] = [];
  private server: tls.Server;

  constructor(public secret: string) {
    this.server = tls.createServer({ key: KEY, cert: CERT }, (socket) => this.serve(socket));
  }

  get port(): number {
    return (this.server.address() as AddressInfo).port;
  }

  async start(port = 0): Promise<void> {
    await new Promise<void>((resolve) => this.server.listen(port, '127.0.0.1', resolve));
  }

  async stop(): Promise<void> {
    await new Promise<void>((resolve) => this.server.close(() => resolve()));
  }

  reset(): void {
    this.sessions = [];
    this.instances = [];
    this.kicks = [];
    this.hellos = [];
    this.pongs = [];
    this.chunkSize = 1000;
    this.pingFirst = false;
  }

  private serve(socket: tls.TLSSocket): void {
    let buf = Buffer.alloc(0);
    let linked = false;
    socket.on('error', () => {});
    socket.on('data', (chunk: Buffer) => {
      buf = Buffer.concat([buf, chunk]);
      while (buf.length >= 4) {
        const len = buf.readUInt16LE(0);
        if (buf.length < 4 + len) return;
        const op = buf.readUInt16LE(2);
        const p = buf.subarray(4, 4 + len);
        buf = buf.subarray(4 + len);
        if (op === 0x0004) {
          this.pongs.push(p.readUInt32LE(0));
          continue;
        }
        if (!linked) {
          if (op !== 0x0200) return void socket.end(disconnect(14));
          const version = p.readUInt16LE(0);
          const secret = p.subarray(4, 4 + p.readUInt16LE(2)).toString('utf8');
          this.hellos.push({ version, secret });
          if (version !== 10) return void socket.end(disconnect(1));
          if (secret !== this.secret) return void socket.end(disconnect(8));
          linked = true;
          socket.write(frame(0x0201));
          continue;
        }
        if (this.pingFirst) socket.write(frame(0x0003, Buffer.from([0x78, 0x56, 0x34, 0x12])));
        if (op === 0x0300) this.sendOnline(socket);
        else if (op === 0x0302) this.kick(socket, p);
        else if (op === 0x0304) this.sendInstances(socket);
        else return void socket.end(disconnect(3));
      }
    });
  }

  private sendOnline(socket: tls.TLSSocket): void {
    let i = 0;
    do {
      const chunk = this.sessions.slice(i, i + this.chunkSize);
      i += chunk.length;
      const parts = [Buffer.from([i < this.sessions.length ? 1 : 0]), Buffer.alloc(2)];
      parts[1]!.writeUInt16LE(chunk.length);
      for (const s of chunk) {
        const b = Buffer.alloc(8 + 8);
        b.writeBigUInt64LE(BigInt(s.accountId), 0);
        b.writeBigUInt64LE(BigInt(s.charId), 8);
        const name = Buffer.from(s.name, 'utf8');
        const tail = Buffer.alloc(2 + 4 + 8);
        tail.writeUInt16LE(s.map, 0);
        tail.writeUInt32LE(s.sessionId, 2);
        tail.writeBigUInt64LE(BigInt(s.connectedAtMs), 6);
        parts.push(b, Buffer.from([name.length & 0xff, name.length >> 8]), name, tail);
      }
      socket.write(frame(0x0301, Buffer.concat(parts)));
    } while (i < this.sessions.length);
  }

  // AdminInstances: more u8, count u16, then per entry map u16, mapName str, instance u32, key str, players u32,
  // npcs u32, createdAtMs u64, emptySinceMs u64.
  private sendInstances(socket: tls.TLSSocket): void {
    const s = (text: string) => {
      const b = Buffer.from(text, 'utf8');
      return Buffer.concat([Buffer.from([b.length & 0xff, b.length >> 8]), b]);
    };
    let i = 0;
    do {
      const chunk = this.instances.slice(i, i + this.chunkSize);
      i += chunk.length;
      const parts = [Buffer.from([i < this.instances.length ? 1 : 0]), Buffer.alloc(2)];
      parts[1]!.writeUInt16LE(chunk.length);
      for (const e of chunk) {
        const map = Buffer.alloc(2);
        map.writeUInt16LE(e.map);
        const instance = Buffer.alloc(4);
        instance.writeUInt32LE(e.instance);
        const tail = Buffer.alloc(4 + 4 + 8 + 8);
        tail.writeUInt32LE(e.players, 0);
        tail.writeUInt32LE(e.npcs, 4);
        tail.writeBigUInt64LE(BigInt(e.createdAtMs), 8);
        tail.writeBigUInt64LE(BigInt(e.emptySinceMs), 16);
        parts.push(map, s(e.mapName), instance, s(e.key), tail);
      }
      socket.write(frame(0x0305, Buffer.concat(parts)));
    } while (i < this.instances.length);
  }

  private kick(socket: tls.TLSSocket, p: Buffer): void {
    const accountId = p.readBigUInt64LE(0);
    const charId = p.readBigUInt64LE(8);
    const reason = p.subarray(18, 18 + p.readUInt16LE(16)).toString('utf8');
    if ((accountId === 0n) === (charId === 0n)) return void socket.end(disconnect(2));
    this.kicks.push({ accountId, charId, reason });
    const hit = (s: FakeSession) =>
      accountId ? BigInt(s.accountId) === accountId : BigInt(s.charId) === charId && s.charId !== 0;
    const kicked = this.sessions.filter(hit).length;
    this.sessions = this.sessions.filter((s) => !hit(s));
    const out = Buffer.alloc(4);
    out.writeUInt32LE(kicked);
    socket.write(frame(0x0303, out));
  }
}
