import type { Pool } from 'mysql2/promise';
import type { AuthClient } from '../authClient.js';
import type { Clock } from '../clock.js';
import { AppError, realmUnreachable } from '../errors.js';
import { listRealmEndpoints, replaceRealmEndpoints, type RealmEndpoint } from '../repositories/realmEndpoints.js';
import type { SessionContext } from './session.js';
import type { InstanceEntry, KickTarget, OnlineEntry, WorldAdminClient } from '../worldAdmin.js';

/** Auth's admin realm view. */
export interface Realm extends RealmEndpoint {
  host: string;
  port: number;
  region: string;
  status: string;
  sortOrder: number;
}

export interface KickReport {
  kicked: number;
  /** Realms that could not be asked; the rest of the operation went ahead. */
  failures: { realmId: number; code: string }[];
}

export type RealmService = ReturnType<typeof createRealmService>;

export function createRealmService(deps: { auth: AuthClient; world: WorldAdminClient; pool: Pool; clock: Clock }) {
  const { auth, world, pool, clock } = deps;

  /** Admin only (auth enforces it). Refreshes the endpoint copy GM requests rely on. */
  async function list(token: string): Promise<Realm[]> {
    const { realms } = await auth.request<{ realms: Realm[] }>('/v1/admin/realms', { token });
    await replaceRealmEndpoints(pool, realms, clock.now());
    return realms;
  }

  /** Admins read auth live; GMs can't, so they get the copy from the last admin read. */
  const endpoints = (ctx: SessionContext): Promise<RealmEndpoint[]> =>
    ctx.caller.role === 'admin' ? list(ctx.token) : listRealmEndpoints(pool);

  async function endpoint(ctx: SessionContext, realmId: number): Promise<{ host: string; port: number }> {
    const realm = (await endpoints(ctx)).find((r) => r.id === realmId);
    // GMs read the copy, which may lag auth: a realm missing from it is unreachable to them, not unknown.
    if (!realm && ctx.caller.role !== 'admin') throw realmUnreachable('The realm is not known to the admin service yet');
    if (!realm) throw new AppError(404, 'realm_not_found', 'Realm not found');
    if (!realm.adminHost || !realm.adminPort) throw realmUnreachable('The realm has no admin endpoint configured');
    return { host: realm.adminHost, port: realm.adminPort };
  }

  const reachable = async (r: RealmEndpoint): Promise<boolean> => {
    if (!r.adminHost || !r.adminPort) return false;
    return world.online({ host: r.adminHost, port: r.adminPort }).then(
      () => true,
      () => false,
    );
  };

  return {
    list,
    endpoints,

    /** Every realm with whether its world admin port answers right now. */
    async listWithStatus<T extends RealmEndpoint>(realms: T[]): Promise<(T & { reachable: boolean })[]> {
      return Promise.all(realms.map(async (r) => ({ ...r, reachable: await reachable(r) })));
    },

    online: async (ctx: SessionContext, realmId: number): Promise<OnlineEntry[]> =>
      world.online(await endpoint(ctx, realmId)),

    instances: async (ctx: SessionContext, realmId: number): Promise<InstanceEntry[]> =>
      world.instances(await endpoint(ctx, realmId)),

    kick: async (ctx: SessionContext, realmId: number, target: KickTarget, reason: string): Promise<number> =>
      world.kick(await endpoint(ctx, realmId), target, reason),

    /** True when any session of accountId is on the realm (lobby or world). 503 when it can't be asked. */
    async isAccountOnline(ctx: SessionContext, realmId: number, accountId: number): Promise<boolean> {
      const online = await world.online(await endpoint(ctx, realmId));
      return online.some((e) => e.accountId === accountId);
    },

    /** Kicks the account on every realm; a realm that can't be reached is reported, not fatal. */
    async kickEverywhere(ctx: SessionContext, accountId: number, reason: string): Promise<KickReport> {
      const report: KickReport = { kicked: 0, failures: [] };
      for (const realm of await endpoints(ctx)) {
        try {
          if (!realm.adminHost || !realm.adminPort) throw realmUnreachable();
          report.kicked += await world.kick({ host: realm.adminHost, port: realm.adminPort }, { accountId }, reason);
        } catch (err) {
          report.failures.push({ realmId: realm.id, code: err instanceof AppError ? err.code : 'internal_error' });
        }
      }
      return report;
    },
  };
}
