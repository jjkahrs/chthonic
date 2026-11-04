import type { Pool } from 'mysql2/promise';
import type { Clock } from '../clock.js';
import { addHit, pruneExpiredHits } from '../repositories/rateLimits.js';

export interface RateLimitRule {
  bucket: string;
  limit: number;
  windowMs: number;
}

const MINUTE = 60_000;
const HOUR = 60 * MINUTE;

export const RATE_LIMITS = {
  login: { bucket: 'login', limit: 20, windowMs: MINUTE },
  register: { bucket: 'register', limit: 5, windowMs: HOUR },
  forgotPassword: { bucket: 'forgot_password', limit: 5, windowMs: HOUR },
  resendVerification: { bucket: 'resend_verification', limit: 5, windowMs: HOUR },
} satisfies Record<string, RateLimitRule>;

export interface RateLimitResult {
  allowed: boolean;
  retryAfterSeconds: number;
}

export type RateLimiter = ReturnType<typeof createRateLimiter>;

/**
 * Fixed-window counters in MySQL, so limits survive restarts and are shared by every instance.
 * ponytail: a client can burst up to 2x the limit across a window boundary; switch to a sliding
 * window if that ever matters.
 */
export function createRateLimiter(deps: { pool: Pool; clock: Clock }) {
  const { pool, clock } = deps;

  return {
    async hit(rule: RateLimitRule, ip: string): Promise<RateLimitResult> {
      const now = clock.now();
      const windowStart = Math.floor(now.getTime() / rule.windowMs) * rule.windowMs;
      const windowEnd = windowStart + rule.windowMs;
      await pruneExpiredHits(pool, now);
      const hits = await addHit(pool, rule.bucket, ip, new Date(windowStart), new Date(windowEnd));
      return {
        allowed: hits <= rule.limit,
        retryAfterSeconds: Math.max(1, Math.ceil((windowEnd - now.getTime()) / 1000)),
      };
    },
  };
}
