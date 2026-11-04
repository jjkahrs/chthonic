/** The admin API's {error: {code, message, details}} as a throwable. */
export class ApiError extends Error {
  constructor(
    readonly status: number,
    readonly code: string,
    message: string,
    readonly details?: unknown,
  ) {
    super(message);
  }
}

export type Role = 'player' | 'gm' | 'admin';
export interface Caller {
  id: number;
  email: string;
  role: Role;
}

export interface Ban {
  id: number;
  reason: string;
  createdAt: string;
  expiresAt: string | null;
  bannedBy: number | null;
  liftedAt?: string | null;
  liftedBy?: number | null;
}
export interface Account {
  id: number;
  email: string;
  role: Role;
  emailVerified: boolean;
  emailVerifiedAt: string | null;
  failedLoginCount: number;
  lockedUntil: string | null;
  createdAt: string;
  activeBan: Ban | null;
  bans?: Ban[];
}
/** How kicking an account's online characters went, per realm. */
export interface Kicks {
  kicked: number;
  failures: { realmId: number; code: string }[];
}
export interface Character {
  id: number;
  realmId: number;
  accountId: number;
  name: string;
  mapId: number;
  createdAt: string;
  lastLoginAt: string | null;
  deletedAt: string | null;
}
export interface CharacterDetail extends Character {
  position: { x: number; y: number; z: number };
  heading: number;
  components: Record<string, unknown>;
  options: Record<string, unknown>;
}
/** GMs get only id, name and reachable. */
export interface Realm {
  id: number;
  name: string;
  reachable: boolean;
  host?: string;
  port?: number;
  adminHost?: string | null;
  adminPort?: number | null;
  region?: string;
  status?: string;
  sortOrder?: number;
}
export interface OnlineEntry {
  accountId: number;
  characterId: number;
  characterName: string;
  mapId: number;
  sessionId: number;
  connectedAt: string;
}
/** A live map context: a world map's (instanceId 0) or an instance copy's. */
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
export interface AuditEntry {
  id: number;
  actorAccountId: number;
  actorEmail: string;
  actorRole: string;
  action: string;
  targetType: string;
  targetId: string | null;
  before: unknown;
  after: unknown;
  forced: boolean;
  result: string;
  createdAt: string;
}

/** The CSRF cookie is JS-readable on purpose: every non-GET echoes it in X-CSRF-Token. */
function csrfToken(): string {
  const prefix = 'chthonic_admin_csrf=';
  return document.cookie.split('; ').find((c) => c.startsWith(prefix))?.slice(prefix.length) ?? '';
}

type Query = Record<string, string | number | boolean | undefined>;

export async function api<T>(
  path: string,
  opts: { method?: 'GET' | 'POST' | 'PUT' | 'DELETE'; body?: unknown; query?: Query } = {},
): Promise<T> {
  const { method = 'GET', body, query } = opts;
  const params = new URLSearchParams();
  for (const [k, v] of Object.entries(query ?? {})) if (v !== undefined && v !== '') params.set(k, String(v));
  const qs = params.size ? `?${params}` : '';
  const headers: Record<string, string> = {};
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  if (method !== 'GET') headers['X-CSRF-Token'] = csrfToken();
  const res = await fetch(`/api${path}${qs}`, {
    method,
    headers,
    body: body === undefined ? undefined : JSON.stringify(body),
    credentials: 'same-origin',
  });
  if (res.status === 204) return undefined as T;
  const data = await res.json().catch(() => null);
  if (!res.ok) {
    const e = data?.error;
    throw new ApiError(res.status, e?.code ?? `http_${res.status}`, e?.message ?? res.statusText, e?.details);
  }
  return data as T;
}

export const isCode = (err: unknown, code: string): boolean => err instanceof ApiError && err.code === code;
