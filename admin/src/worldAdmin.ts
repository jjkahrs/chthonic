import { isIP } from 'node:net';
import tls from 'node:tls';
import { realmUnreachable } from './errors.js';

// World admin port protocol (docs/world/protocol.html#admin): TLS, frames of
// u16 payloadLength, u16 opcode, payload (little-endian); str = u16 byte length + UTF-8 bytes.
const PROTOCOL_VERSION = 10; // the admin port's own (kAdminProtocolVersion): 10 added the instance list
export const OP = {
  ping: 0x0003,
  pong: 0x0004,
  disconnect: 0x0103,
  linkHello: 0x0200,
  linkOk: 0x0201,
  onlineReq: 0x0300,
  online: 0x0301,
  kick: 0x0302,
  kicked: 0x0303,
  instancesReq: 0x0304,
  instances: 0x0305,
} as const;

export interface Endpoint {
  host: string;
  port: number;
}

/** A session on a world server: in the lobby (characterId 0, name '', mapId 0) or in the world. */
export interface OnlineEntry {
  accountId: number;
  characterId: number;
  characterName: string;
  mapId: number;
  sessionId: number;
  connectedAt: string;
}

/**
 * A live map context on a world server: a world map's (instanceId 0, key '') or an instance copy's
 * (docs/world/instances.html). emptySince: when an empty copy's timer started; null while it has players.
 */
export interface InstanceEntry {
  mapId: number;
  mapName: string;
  instanceId: number;
  key: string;
  players: number;
  npcs: number;
  createdAt: string;
  emptySince: string | null;
}

export type KickTarget = { accountId: number; characterId?: never } | { characterId: number; accountId?: never };

export interface WorldAdminClient {
  /** Every failure (no TLS, refused, timeout, wrong secret, closed early) throws 503 realm_unreachable. */
  online(endpoint: Endpoint): Promise<OnlineEntry[]>;
  /** Returns how many sessions were disconnected. */
  kick(endpoint: Endpoint, target: KickTarget, reason: string): Promise<number>;
  /** Every live map context, in map then instance order. */
  instances(endpoint: Endpoint): Promise<InstanceEntry[]>;
}

export function encodeFrame(op: number, payload: Buffer = Buffer.alloc(0)): Buffer {
  const header = Buffer.alloc(4);
  header.writeUInt16LE(payload.length, 0);
  header.writeUInt16LE(op, 2);
  return Buffer.concat([header, payload]);
}

const u16 = (v: number) => Buffer.from([v & 0xff, v >> 8]);
const u64 = (v: number) => {
  const b = Buffer.alloc(8);
  b.writeBigUInt64LE(BigInt(v));
  return b;
};
const str = (s: string) => {
  const bytes = Buffer.from(s, 'utf8');
  return Buffer.concat([u16(bytes.length), bytes]);
};

/** Reads a payload front to back; anything short or left over is a protocol error. */
class Reader {
  private offset = 0;
  constructor(private readonly buf: Buffer) {}
  u8 = () => this.take(1).readUInt8(0);
  u16 = () => this.take(2).readUInt16LE(0);
  u32 = () => this.take(4).readUInt32LE(0);
  u64 = () => Number(this.take(8).readBigUInt64LE(0));
  str = () => this.take(this.u16()).toString('utf8');
  end(): void {
    if (this.offset !== this.buf.length) throw new Error('payload has trailing bytes');
  }
  private take(n: number): Buffer {
    if (this.offset + n > this.buf.length) throw new Error('payload too short');
    return this.buf.subarray(this.offset, (this.offset += n));
  }
}

interface Frame {
  op: number;
  payload: Buffer;
}

/** One TLS connection that has passed LinkHello; recv() answers pings itself. */
class Link {
  private buffered = Buffer.alloc(0);
  private readonly frames: Frame[] = [];
  private failure: Error | undefined;
  private waiter: { resolve: (f: Frame) => void; reject: (e: Error) => void } | undefined;

  constructor(private readonly socket: tls.TLSSocket) {
    socket.on('data', (chunk: Buffer) => {
      this.buffered = Buffer.concat([this.buffered, chunk]);
      while (this.buffered.length >= 4) {
        const end = 4 + this.buffered.readUInt16LE(0);
        if (this.buffered.length < end) break;
        this.frames.push({ op: this.buffered.readUInt16LE(2), payload: this.buffered.subarray(4, end) });
        this.buffered = this.buffered.subarray(end);
      }
      this.settle();
    });
    socket.on('error', (err) => this.fail(err));
    socket.on('close', () => this.fail(new Error('connection closed')));
  }

  send(op: number, payload?: Buffer): void {
    this.socket.write(encodeFrame(op, payload));
  }

  /** The next frame with opcode op; a Disconnect or any other opcode is an error. */
  async recv(op: number): Promise<Reader> {
    for (;;) {
      const frame = await new Promise<Frame>((resolve, reject) => {
        this.waiter = { resolve, reject };
        this.settle();
      });
      const reader = new Reader(frame.payload);
      if (frame.op === OP.ping) {
        this.send(OP.pong, frame.payload); // same u32 nonce back
        continue;
      }
      if (frame.op === OP.disconnect) throw new Error(`disconnected by world server (reason ${reader.u16()})`);
      if (frame.op !== op) throw new Error(`unexpected opcode 0x${frame.op.toString(16)}`);
      return reader;
    }
  }

  fail(err: Error): void {
    this.failure ??= err;
    this.settle();
  }

  private settle(): void {
    const waiter = this.waiter;
    if (!waiter) return;
    const frame = this.frames.shift();
    if (frame) {
      this.waiter = undefined;
      waiter.resolve(frame);
    } else if (this.failure) {
      this.waiter = undefined;
      waiter.reject(this.failure);
    }
  }
}

export function createWorldAdminClient(deps: {
  secret: string;
  ca?: string | Buffer;
  timeoutMs: number;
  log?: (msg: string) => void;
}): WorldAdminClient {
  const { secret, ca, timeoutMs, log = () => {} } = deps;

  /** Opens a connection, says hello, runs work, closes. One connection per request. */
  async function withLink<T>(endpoint: Endpoint, work: (link: Link) => Promise<T>): Promise<T> {
    const { host, port } = endpoint;
    // Node warns when servername is an IP; certificates for IPs are checked against IP SANs anyway.
    const socket = tls.connect({ host, port, ca, servername: isIP(host) ? undefined : host });
    const link = new Link(socket);
    const timer = setTimeout(() => link.fail(new Error(`timed out after ${timeoutMs} ms`)), timeoutMs);
    try {
      link.send(OP.linkHello, Buffer.concat([u16(PROTOCOL_VERSION), str(secret)]));
      (await link.recv(OP.linkOk)).end();
      return await work(link);
    } catch (err) {
      // Never log the secret; the error messages here don't contain it.
      log(`World admin ${host}:${port} unreachable: ${(err as Error).message}`);
      throw realmUnreachable();
    } finally {
      clearTimeout(timer);
      socket.destroy();
    }
  }

  return {
    online: (endpoint) =>
      withLink(endpoint, async (link) => {
        link.send(OP.onlineReq);
        const entries: OnlineEntry[] = [];
        for (let more = true; more; ) {
          const r = await link.recv(OP.online);
          more = r.u8() === 1;
          for (let n = r.u16(); n > 0; n--) {
            entries.push({
              accountId: r.u64(),
              characterId: r.u64(),
              characterName: r.str(),
              mapId: r.u16(),
              sessionId: r.u32(),
              connectedAt: new Date(r.u64()).toISOString(),
            });
          }
          r.end();
        }
        return entries;
      }),

    instances: (endpoint) =>
      withLink(endpoint, async (link) => {
        link.send(OP.instancesReq);
        const entries: InstanceEntry[] = [];
        for (let more = true; more; ) {
          const r = await link.recv(OP.instances);
          more = r.u8() === 1;
          for (let n = r.u16(); n > 0; n--) {
            const mapId = r.u16();
            const mapName = r.str();
            const instanceId = r.u32();
            const key = r.str();
            const players = r.u32();
            const npcs = r.u32();
            const createdAt = new Date(r.u64()).toISOString();
            const emptySinceMs = r.u64();
            entries.push({
              mapId,
              mapName,
              instanceId,
              key,
              players,
              npcs,
              createdAt,
              emptySince: emptySinceMs ? new Date(emptySinceMs).toISOString() : null,
            });
          }
          r.end();
        }
        return entries;
      }),

    kick: (endpoint, target, reason) =>
      withLink(endpoint, async (link) => {
        link.send(OP.kick, Buffer.concat([u64(target.accountId ?? 0), u64(target.characterId ?? 0), str(reason)]));
        const r = await link.recv(OP.kicked);
        const kicked = r.u32();
        r.end();
        return kicked;
      }),
  };
}
