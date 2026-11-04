import { AppError } from './errors.js';

/** Auth's POST /v1/auth/login and /refresh answer. */
export interface AuthSession {
  accessToken: string;
  refreshToken: string;
  refreshTokenExpiresAt: string;
}

export interface AuthRequest {
  method?: 'GET' | 'POST' | 'PUT' | 'DELETE';
  token?: string;
  body?: unknown;
  query?: Record<string, string | number | boolean | undefined>;
}

export type AuthClient = ReturnType<typeof createAuthClient>;

const TIMEOUT_MS = 10_000;

/**
 * Talks to the auth service. Auth's own errors are rethrown unchanged ({status, code, message, details}),
 * so the SPA sees auth's codes; a network failure is 502 auth_unavailable.
 */
export function createAuthClient(baseUrl: string) {
  async function request<T>(path: string, opts: AuthRequest = {}): Promise<T> {
    const url = new URL(path, baseUrl);
    for (const [k, v] of Object.entries(opts.query ?? {})) if (v !== undefined) url.searchParams.set(k, String(v));
    let res: Response;
    try {
      res = await fetch(url, {
        method: opts.method ?? 'GET',
        headers: {
          ...(opts.token ? { Authorization: `Bearer ${opts.token}` } : {}),
          ...(opts.body === undefined ? {} : { 'Content-Type': 'application/json' }),
        },
        body: opts.body === undefined ? undefined : JSON.stringify(opts.body),
        signal: AbortSignal.timeout(TIMEOUT_MS),
      });
    } catch {
      throw new AppError(502, 'auth_unavailable', 'The auth service is unreachable');
    }
    if (res.status === 204) return undefined as T;
    const body = (await res.json().catch(() => undefined)) as
      | { error?: { code?: string; message?: string; details?: unknown } }
      | undefined;
    if (!res.ok) {
      const e = body?.error;
      if (!e?.code) throw new AppError(502, 'auth_unavailable', 'The auth service answered unexpectedly');
      throw new AppError(res.status, e.code, e.message ?? e.code, e.details);
    }
    return body as T;
  }

  return {
    baseUrl,
    request,
    login: (email: string, password: string) =>
      request<AuthSession>('/v1/auth/login', { method: 'POST', body: { email, password } }),
    refresh: (refreshToken: string) =>
      request<AuthSession>('/v1/auth/refresh', { method: 'POST', body: { refreshToken } }),
    logout: (refreshToken: string) => request<void>('/v1/auth/logout', { method: 'POST', body: { refreshToken } }),
  };
}
