import { randomBytes, timingSafeEqual } from 'node:crypto';
import type { ErrorRequestHandler, RequestHandler, Request, Response } from 'express';
import type { z } from 'zod';
import type { AuthSession } from '../authClient.js';
import { AppError, forbidden, unauthorized } from '../errors.js';
import type { Logger } from '../logger.js';
import { TokenExpired, type Role, type SessionContext, type SessionService } from '../services/session.js';

export const COOKIES = { access: 'chthonic_admin_access', refresh: 'chthonic_admin_refresh', csrf: 'chthonic_admin_csrf' } as const;
export const CSRF_HEADER = 'X-CSRF-Token';

/** Parses untrusted input or throws 400 validation_error listing every problem. */
export function parse<S extends z.ZodType>(schema: S, input: unknown): z.output<S> {
  const result = schema.safeParse(input);
  if (!result.success) {
    throw new AppError(
      400,
      'validation_error',
      'Request validation failed',
      result.error.issues.map((i) => ({ path: i.path.join('.'), message: i.message })),
    );
  }
  return result.data;
}

export function readCookies(req: Request): Record<string, string> {
  const out: Record<string, string> = {};
  for (const part of req.get('cookie')?.split(';') ?? []) {
    const eq = part.indexOf('=');
    if (eq > 0) out[part.slice(0, eq).trim()] = decodeURIComponent(part.slice(eq + 1).trim());
  }
  return out;
}

/** Session cookies: the SPA's JavaScript never sees a token. */
export function cookieWriter(secure: boolean) {
  const base = { httpOnly: true, sameSite: 'strict', secure, path: '/api' } as const;
  return {
    setSession(res: Response, session: AuthSession): void {
      const expires = new Date(session.refreshTokenExpiresAt);
      res.cookie(COOKIES.access, session.accessToken, { ...base, expires });
      res.cookie(COOKIES.refresh, session.refreshToken, { ...base, expires });
    },
    clearSession(res: Response): void {
      res.clearCookie(COOKIES.access, base);
      res.clearCookie(COOKIES.refresh, base);
    },
    /** Double-submit token: readable by the SPA, which echoes it in X-CSRF-Token. */
    setCsrf(res: Response): void {
      res.cookie(COOKIES.csrf, randomBytes(32).toString('base64url'), { sameSite: 'strict', secure, path: '/' });
    },
  };
}

export type CookieWriter = ReturnType<typeof cookieWriter>;

const SAFE_METHODS = new Set(['GET', 'HEAD', 'OPTIONS']);

/** Hands out a CSRF cookie on first contact; every other method must echo it in the header. */
export function csrf(cookies: CookieWriter): RequestHandler {
  return (req, res, next) => {
    const token = readCookies(req)[COOKIES.csrf];
    if (!token) cookies.setCsrf(res);
    if (SAFE_METHODS.has(req.method)) return next();
    const header = req.get(CSRF_HEADER) ?? '';
    const a = Buffer.from(header);
    const b = Buffer.from(token ?? '');
    if (!token || a.length !== b.length || !timingSafeEqual(a, b)) {
      throw new AppError(403, 'csrf_invalid', 'Missing or invalid CSRF token');
    }
    next();
  };
}

export const sessionOf = (res: Response): SessionContext => res.locals.session as SessionContext;

/**
 * Requires a signed-in admin or GM; refreshes an expired access token through auth on the way.
 * Writes (and every request when alwaysRecheck is set) take the caller's role from auth's /v1/me, not the token,
 * so a demoted, banned or deleted caller loses its powers within RECHECK_MS.
 */
export function requireSession(
  sessions: SessionService,
  cookies: CookieWriter,
  opts: { alwaysRecheck?: boolean } = {},
): RequestHandler {
  return async (req, res, next) => {
    const { [COOKIES.access]: access, [COOKIES.refresh]: refresh } = readCookies(req);
    let context: SessionContext | undefined;
    if (access) {
      try {
        context = { caller: await sessions.verify(access), token: access };
      } catch (err) {
        if (!(err instanceof TokenExpired)) throw err;
      }
    }
    if (!context) {
      if (!refresh) throw unauthorized('Not signed in');
      try {
        const session = await sessions.refresh(refresh);
        context = { caller: await sessions.verify(session.accessToken), token: session.accessToken };
        cookies.setSession(res, session);
      } catch {
        cookies.clearSession(res);
        throw unauthorized('Session expired; sign in again');
      }
    }
    if (opts.alwaysRecheck || !SAFE_METHODS.has(req.method)) {
      try {
        context.caller = await sessions.recheck(context.token);
      } catch (err) {
        if (err instanceof AppError && err.status === 401) cookies.clearSession(res);
        throw err;
      }
    }
    if (context.caller.role === 'player') {
      // As at login: a player's session is ended, not kept around.
      if (refresh) await sessions.logout(refresh).catch(() => {});
      cookies.clearSession(res);
      throw forbidden('Only admins and GMs can use the admin app');
    }
    res.locals.session = context;
    next();
  };
}

/** Lets through only callers whose role is one of roles. Runs after requireSession. */
export function requireRole(...roles: Role[]): RequestHandler {
  return (_req, res, next) => {
    if (!roles.includes(sessionOf(res).caller.role)) throw forbidden();
    next();
  };
}

export const noStore: RequestHandler = (_req, res, next) => {
  res.set('Cache-Control', 'no-store');
  next();
};

export const notFoundHandler: RequestHandler = () => {
  throw new AppError(404, 'not_found', 'Route not found');
};

/** Maps every error to {error:{code,message,details?}}. Unknown errors never leak internals. */
export function errorHandler(log: Logger): ErrorRequestHandler {
  return (err, _req, res, _next) => {
    const appError = toAppError(err);
    if (!appError) {
      // Log name/code/message/stack only: mysql2 errors carry the full SQL with bound values.
      const e = err as { name?: string; code?: string; message?: string; stack?: string };
      log.error(`Unhandled ${e.name ?? 'error'}${e.code ? ` (${e.code})` : ''}: ${e.message}\n${e.stack ?? ''}`);
    }
    const { status, code, message, details } = appError ?? new AppError(500, 'internal_error', 'Internal server error');
    res.status(status).json({ error: { code, message, ...(details === undefined ? {} : { details }) } });
  };
}

export function toAppError(err: unknown): AppError | undefined {
  if (err instanceof AppError) return err;
  // Errors raised by express.json() before our handlers run.
  const type = (err as { type?: string }).type;
  if (type === 'entity.parse.failed') return new AppError(400, 'validation_error', 'Request body is not valid JSON');
  if (type === 'entity.too.large') return new AppError(413, 'payload_too_large', 'Request body is too large');
  const status = (err as { status?: number }).status;
  if (type && status && status >= 400 && status < 500) return new AppError(status, 'bad_request', 'Malformed request');
  return undefined;
}
