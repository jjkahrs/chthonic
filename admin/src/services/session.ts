import { createRemoteJWKSet, errors, jwtVerify, type JWTVerifyGetKey } from 'jose';
import type { AuthClient, AuthSession } from '../authClient.js';
import type { Clock } from '../clock.js';
import { AppError, forbidden, unauthorized } from '../errors.js';

export const ROLES = ['player', 'gm', 'admin'] as const;
export type Role = (typeof ROLES)[number];

/** The signed-in admin or GM, from the access token's claims. */
export interface Caller {
  id: number;
  email: string;
  role: Role;
}

export interface SessionContext {
  caller: Caller;
  /** The caller's auth access token, for requests to auth on their behalf. */
  token: string;
}

export type SessionService = ReturnType<typeof createSessionService>;

/** Expired-but-genuine tokens get this, so the caller knows a refresh may help. */
export class TokenExpired extends Error {}

// Concurrent requests with the same expired token share one refresh: auth rotates refresh tokens and treats a reused
// one as theft, so two parallel refreshes would end the session.
const REFRESH_SHARE_MS = 10_000;
// How long a /v1/me answer stands in for auth: the most a demoted, banned or deleted caller keeps its powers.
export const RECHECK_MS = 5_000;
const RECHECK_CACHE_MAX = 10_000;

export function createSessionService(deps: {
  auth: AuthClient;
  clock: Clock;
  issuer: string;
  audience: string;
  /** Defaults to auth's published JWK Set. */
  jwks?: JWTVerifyGetKey;
}) {
  const { auth, clock, issuer, audience } = deps;
  const jwks = deps.jwks ?? createRemoteJWKSet(new URL('/.well-known/jwks.json', auth.baseUrl));
  const refreshing = new Map<string, Promise<AuthSession>>();
  const rechecked = new Map<string, { caller: Caller; until: number }>();

  /** Verifies an access token locally; throws TokenExpired or 401. */
  async function verify(token: string): Promise<Caller> {
    try {
      const { payload } = await jwtVerify(token, jwks, {
        issuer,
        audience,
        algorithms: ['RS256'],
        currentDate: clock.now(),
        requiredClaims: ['sub', 'exp', 'iat'],
      });
      const id = Number(payload.sub);
      const role = payload.role as Role;
      if (!Number.isSafeInteger(id) || typeof payload.email !== 'string' || !ROLES.includes(role)) {
        throw new Error('malformed claims');
      }
      return { id, email: payload.email, role };
    } catch (err) {
      if (err instanceof errors.JWTExpired) throw new TokenExpired();
      throw unauthorized();
    }
  }

  /**
   * The caller as auth sees it now (role, ban, deletion), cached per token for RECHECK_MS.
   * Throws 401 when auth refuses the token and passes auth_unavailable through: never falls back to the claims.
   */
  async function recheck(token: string): Promise<Caller> {
    const now = clock.now().getTime();
    const hit = rechecked.get(token);
    if (hit && now < hit.until) return hit.caller;
    let me: Awaited<ReturnType<AuthClient['me']>>;
    try {
      me = await auth.me(token);
    } catch (err) {
      if ((err as { status?: number }).status === 401) throw unauthorized('Your account is no longer active; sign in again');
      throw err;
    }
    const role = me.role as Role;
    if (!ROLES.includes(role)) throw new AppError(502, 'auth_unavailable', 'The auth service answered unexpectedly');
    const caller = { id: me.id, email: me.email, role };
    if (rechecked.size >= RECHECK_CACHE_MAX) {
      for (const [k, v] of rechecked) if (now >= v.until) rechecked.delete(k);
      // ponytail: still full means a flood of live tokens; dropping the cache only costs extra /v1/me calls.
      if (rechecked.size >= RECHECK_CACHE_MAX) rechecked.clear();
    }
    rechecked.set(token, { caller, until: now + RECHECK_MS });
    return caller;
  }

  return {
    verify,
    recheck,

    /** Signs in through auth; players are turned away and their new session is ended at once. */
    async login(email: string, password: string): Promise<{ caller: Caller; session: AuthSession }> {
      const session = await auth.login(email, password);
      const caller = await verify(session.accessToken);
      if (caller.role === 'player') {
        await auth.logout(session.refreshToken).catch(() => {});
        throw forbidden('Only admins and GMs can use the admin app');
      }
      return { caller, session };
    },

    refresh(refreshToken: string): Promise<AuthSession> {
      let pending = refreshing.get(refreshToken);
      if (!pending) {
        pending = auth.refresh(refreshToken);
        refreshing.set(refreshToken, pending);
        // ponytail: in-process only; several admin instances behind a balancer would need sticky sessions.
        setTimeout(() => refreshing.delete(refreshToken), REFRESH_SHARE_MS).unref();
      }
      return pending;
    },

    logout: (refreshToken: string) => auth.logout(refreshToken),
  };
}
