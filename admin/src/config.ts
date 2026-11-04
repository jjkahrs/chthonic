import { z } from 'zod';

/** Unset or empty: fallback. */
const bool = (fallback: 'true' | 'false') =>
  z.preprocess((v) => (v === '' ? undefined : v), z.enum(['true', 'false']).default(fallback)).transform((v) => v === 'true');
/** Treats unset and empty-string env vars the same. */
const optional = z
  .string()
  .optional()
  .transform((v) => (v === undefined || v.trim() === '' ? undefined : v));
const port = z.coerce.number().int().min(1).max(65535);

const schema = z.object({
  PORT: port.default(3200),
  TRUST_PROXY: z.string().default('false'),
  /** Off only for plain-http local development: browsers drop Secure cookies on http other than localhost. */
  COOKIE_SECURE: bool('true'),
  /** The built SPA; unset = admin/web/dist. */
  STATIC_DIR: optional,

  AUTH_URL: z.url(),
  JWT_ISSUER: z.string().min(1),
  JWT_AUDIENCE: z.string().min(1),

  DB_HOST: z.string().min(1),
  DB_PORT: port.default(3306),
  // chthonic_admin: the audit log; this user owns that database only.
  ADMIN_DB_USER: z.string().min(1),
  ADMIN_DB_PASSWORD: z.string().min(1),
  ADMIN_DB_NAME: z.string().min(1),
  // chthonic_world: this user has SELECT and UPDATE on characters only.
  WORLD_DB_USER: z.string().min(1),
  WORLD_DB_PASSWORD: z.string().min(1),
  WORLD_DB_NAME: z.string().min(1),

  WORLD_ADMIN_SECRET: z.string().min(16),
  /** PEM CA that signed the world servers' TLS certs (the dev cert is self-signed); unset = system CAs. */
  WORLD_ADMIN_CA_FILE: optional,
  WORLD_ADMIN_TIMEOUT_MS: z.coerce.number().int().positive().default(3000),
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
