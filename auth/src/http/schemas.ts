import { z } from 'zod';
import { ROLES } from '../repositories/accounts.js';
import { REALM_STATUSES } from '../repositories/realms.js';

/** Emails are case-insensitive: trimmed and lower-cased before anything else sees them. */
const email = z.string().trim().toLowerCase().max(254).pipe(z.email());
const newPassword = z.string().min(10).max(128);
const token = z.string().min(1).max(512);

export const registerBody = z.object({ email, password: newPassword });
// Login doesn't enforce the new-password policy; any wrong password is just invalid_credentials.
export const loginBody = z.object({ email, password: z.string().min(1).max(128) });
export const emailBody = z.object({ email });
export const tokenBody = z.object({ token });
export const refreshBody = z.object({ refreshToken: token });
export const resetPasswordBody = z.object({ token, password: newPassword });

export const idParams = z.object({ id: z.coerce.number().int().positive().max(Number.MAX_SAFE_INTEGER) });

export const banBody = z.object({
  reason: z.string().trim().min(1).max(500),
  expiresAt: z.iso
    .datetime({ offset: true })
    .nullish()
    .transform((v) => (v ? new Date(v) : null)),
});

export const roleBody = z.object({ role: z.enum(ROLES) });

const port = z.number().int().min(1).max(65535);

export const realmBody = z
  .object({
    name: z.string().trim().min(1).max(64),
    host: z.string().trim().min(1).max(255),
    port,
    adminHost: z.string().trim().min(1).max(255).nullish().default(null),
    adminPort: port.nullish().default(null),
    region: z.string().trim().min(1).max(32),
    status: z.enum(REALM_STATUSES).default('offline'),
    sortOrder: z.number().int().min(-2_147_483_648).max(2_147_483_647).default(0),
  })
  .refine((r) => (r.adminHost === null) === (r.adminPort === null), {
    path: ['adminPort'],
    message: 'adminHost and adminPort must be set together',
  });

const queryFlag = z
  .enum(['true', 'false'])
  .transform((v) => v === 'true')
  .optional();

export const accountQuery = z.object({
  q: z.string().trim().max(254).optional(),
  role: z.enum(ROLES).optional(),
  banned: queryFlag,
  verified: queryFlag,
  locked: queryFlag,
  limit: z.coerce.number().int().min(1).max(100).default(50),
  offset: z.coerce.number().int().min(0).max(Number.MAX_SAFE_INTEGER).default(0),
});

export const createAccountBody = z.object({ email, password: newPassword, role: z.enum(ROLES).default('player') });
