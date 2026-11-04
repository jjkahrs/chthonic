import { z } from 'zod';
import { ROLES } from '../services/session.js';

const id = z.coerce.number().int().positive().max(Number.MAX_SAFE_INTEGER);
const limit = z.coerce.number().int().min(1).max(100).default(50);
const offset = z.coerce.number().int().min(0).max(Number.MAX_SAFE_INTEGER).default(0);
const queryFlag = z.enum(['true', 'false']).transform((v) => v === 'true');
const force = z.boolean().default(false);

export const idParams = z.object({ id });

// Session
export const loginBody = z.object({
  email: z.string().trim().toLowerCase().max(254).pipe(z.email()),
  password: z.string().min(1).max(128),
});

// Accounts: passed on to auth, which validates again.
export const accountQuery = z.object({
  q: z.string().trim().max(254).optional(),
  role: z.enum(ROLES).optional(),
  banned: z.enum(['true', 'false']).optional(),
  verified: z.enum(['true', 'false']).optional(),
  locked: z.enum(['true', 'false']).optional(),
  limit,
  offset,
});
export const createAccountBody = z.object({
  email: z.string().trim().toLowerCase().max(254).pipe(z.email()),
  password: z.string().min(10).max(128),
  role: z.enum(ROLES).default('player'),
});
export const banBody = z.object({
  reason: z.string().trim().min(1).max(500),
  expiresAt: z.iso.datetime({ offset: true }).nullish(),
});
export const roleBody = z.object({ role: z.enum(ROLES) });

// Characters
/** Same rule as the world server's valid_character_name: 3-16 ASCII letters. */
export const characterName = z.string().regex(/^[A-Za-z]{3,16}$/, 'Names are 3-16 ASCII letters');
export const characterQuery = z.object({
  realmId: id.optional(),
  accountId: id.optional(),
  name: z
    .string()
    .regex(/^[A-Za-z]{1,16}$/, 'Name prefix is 1-16 ASCII letters')
    .optional(),
  includeDeleted: queryFlag.default(false),
  limit,
  offset,
});
export const forceBody = z.object({ force }).default({ force: false });
export const renameBody = z.object({ name: characterName, force });
const coordinate = z.number().finite();
export const moveBody = z.object({
  mapId: z.number().int().min(0).max(65535),
  x: coordinate,
  y: coordinate,
  z: coordinate,
  heading: coordinate,
  force,
});
export const MAX_JSON_BYTES = 64 * 1024;
const jsonObject = z
  .record(z.string(), z.unknown())
  .refine((v) => Buffer.byteLength(JSON.stringify(v)) <= MAX_JSON_BYTES, `Must be at most ${MAX_JSON_BYTES} bytes`);
export const componentsBody = z.object({ components: jsonObject, force });
export const optionsBody = z.object({ options: jsonObject, force });

// Realms: passed on to auth, which validates again.
const port = z.number().int().min(1).max(65535);
export const realmBody = z.object({
  name: z.string().trim().min(1).max(64),
  host: z.string().trim().min(1).max(255),
  port,
  adminHost: z.string().trim().min(1).max(255).nullish(),
  adminPort: port.nullish(),
  region: z.string().trim().min(1).max(32),
  status: z.enum(['online', 'offline', 'maintenance']).optional(),
  sortOrder: z.number().int().optional(),
});
export const kickBody = z.union([
  z.object({ accountId: id, reason: z.string().trim().min(1).max(200) }).strict(),
  z.object({ characterId: id, reason: z.string().trim().min(1).max(200) }).strict(),
]);

// Audit
export const auditQuery = z.object({
  actorId: id.optional(),
  action: z.string().max(64).optional(),
  targetType: z.string().max(32).optional(),
  targetId: z.string().max(64).optional(),
  from: z.iso.datetime({ offset: true }).transform((v) => new Date(v)).optional(),
  to: z.iso.datetime({ offset: true }).transform((v) => new Date(v)).optional(),
  limit,
  offset,
});
