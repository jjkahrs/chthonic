import { z } from 'zod';

const bool = z.enum(['true', 'false']).default('false').transform((v) => v === 'true');
/** Treats unset and empty-string env vars the same. */
const optional = z
  .string()
  .optional()
  .transform((v) => (v === undefined || v.trim() === '' ? undefined : v));

const schema = z
  .object({
    PORT: z.coerce.number().int().min(1).max(65535).default(3000),
    TRUST_PROXY: z.string().default('false'),
    APP_BASE_URL: z.url(),

    DB_HOST: z.string().min(1),
    DB_PORT: z.coerce.number().int().min(1).max(65535).default(3306),
    DB_USER: z.string().min(1),
    DB_PASSWORD: z.string().min(1),
    DB_NAME: z.string().min(1),

    SMTP_HOST: z.string().min(1),
    SMTP_PORT: z.coerce.number().int().min(1).max(65535),
    SMTP_SECURE: bool,
    SMTP_USER: optional,
    SMTP_PASSWORD: optional,
    MAIL_FROM: z.string().min(1),

    JWT_ISSUER: z.string().min(1),
    JWT_AUDIENCE: z.string().min(1),
    ACCESS_TOKEN_TTL_SECONDS: z.coerce.number().int().positive().default(900),
    REFRESH_TOKEN_TTL_DAYS: z.coerce.number().int().positive().default(30),
    EMAIL_VERIFY_TTL_HOURS: z.coerce.number().int().positive().default(24),
    PASSWORD_RESET_TTL_MINUTES: z.coerce.number().int().positive().default(60),
    JWT_PRIVATE_KEY: optional,
    JWT_PRIVATE_KEY_FILE: optional,
    JWT_KID: optional,
    KEYS_DIR: z.string().default('./keys'),

    SEED_ADMIN_EMAIL: optional.pipe(z.email().optional()),
    SEED_ADMIN_PASSWORD: optional.pipe(z.string().min(10).max(128).optional()),
  })
  .refine((c) => (c.SEED_ADMIN_EMAIL === undefined) === (c.SEED_ADMIN_PASSWORD === undefined), {
    message: 'SEED_ADMIN_EMAIL and SEED_ADMIN_PASSWORD must be set together',
    path: ['SEED_ADMIN_PASSWORD'],
  })
  .refine((c) => !(c.JWT_PRIVATE_KEY && c.JWT_PRIVATE_KEY_FILE), {
    message: 'Set only one of JWT_PRIVATE_KEY and JWT_PRIVATE_KEY_FILE',
    path: ['JWT_PRIVATE_KEY_FILE'],
  });

export type Config = z.infer<typeof schema>;

/** Parses env vars and throws one readable error listing every invalid or missing variable. */
export function loadConfig(env: NodeJS.ProcessEnv = process.env): Config {
  const result = schema.safeParse(env);
  if (!result.success) {
    const lines = result.error.issues.map((i) => `  ${i.path.join('.') || '(root)'}: ${i.message}`);
    throw new Error(`Invalid configuration:\n${lines.join('\n')}`);
  }
  return result.data;
}

/** Express "trust proxy" accepts a boolean, a hop count, or a subnet list. */
export function parseTrustProxy(value: string): boolean | number | string {
  if (value === 'true') return true;
  if (value === 'false') return false;
  return /^\d+$/.test(value) ? Number(value) : value;
}
