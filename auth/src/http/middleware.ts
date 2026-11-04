import type { ErrorRequestHandler, RequestHandler, Response } from 'express';
import type { z } from 'zod';
import { AppError, forbidden, unauthorized } from '../errors.js';
import type { Logger } from '../logger.js';
import type { Role } from '../repositories/accounts.js';
import type { RateLimiter, RateLimitRule } from '../services/rateLimiter.js';
import type { AuthContext, TokenService } from '../services/tokens.js';

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

export function requireAuth(tokens: TokenService): RequestHandler {
  return async (req, res, next) => {
    const [scheme, token] = req.get('authorization')?.split(' ') ?? [];
    if (scheme?.toLowerCase() !== 'bearer' || !token) throw unauthorized();
    res.locals.auth = await tokens.verifyAccessToken(token);
    next();
  };
}

/** The caller set by requireAuth. */
export const authOf = (res: Response): AuthContext => res.locals.auth as AuthContext;

/** Lets through only callers whose token role is one of roles. Runs after requireAuth. */
export function requireRole(...roles: Role[]): RequestHandler {
  return (_req, res, next) => {
    if (!roles.includes(authOf(res).role)) throw forbidden();
    next();
  };
}

export const requireAdmin = requireRole('admin');

export function rateLimit(limiter: RateLimiter, rule: RateLimitRule): RequestHandler {
  return async (req, res, next) => {
    const { allowed, retryAfterSeconds } = await limiter.hit(rule, req.ip ?? 'unknown');
    if (!allowed) {
      res.set('Retry-After', String(retryAfterSeconds));
      throw new AppError(429, 'rate_limited', 'Too many requests; try again later');
    }
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

function toAppError(err: unknown): AppError | undefined {
  if (err instanceof AppError) return err;
  // Errors raised by express.json() before our handlers run.
  const type = (err as { type?: string }).type;
  if (type === 'entity.parse.failed') return new AppError(400, 'validation_error', 'Request body is not valid JSON');
  if (type === 'entity.too.large') return new AppError(413, 'payload_too_large', 'Request body is too large');
  const status = (err as { status?: number }).status;
  if (type && status && status >= 400 && status < 500) return new AppError(status, 'bad_request', 'Malformed request');
  return undefined;
}
