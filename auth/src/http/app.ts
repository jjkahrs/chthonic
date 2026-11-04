import express, { type Express } from 'express';
import helmet from 'helmet';
import type { Pool } from 'mysql2/promise';
import type { Clock } from '../clock.js';
import { parseTrustProxy, type Config } from '../config.js';
import type { SigningKeys } from '../keys.js';
import type { Logger } from '../logger.js';
import type { Mailer } from '../mailer.js';
import { createAdminService } from '../services/admin.js';
import { createAuthService } from '../services/auth.js';
import { createRateLimiter } from '../services/rateLimiter.js';
import { createRealmService } from '../services/realms.js';
import { createTokenService } from '../services/tokens.js';
import { errorHandler, notFoundHandler } from './middleware.js';
import { buildRoutes } from './routes.js';

export interface AppDeps {
  config: Config;
  pool: Pool;
  clock: Clock;
  mailer: Mailer;
  keys: SigningKeys;
  log: Logger;
}

/** Wires services to HTTP. Every external dependency is passed in, so tests can swap any of them. */
export function createApp(deps: AppDeps): Express {
  const { config, pool, clock, mailer, keys, log } = deps;
  const tokens = createTokenService({ config, clock, keys });
  const auth = createAuthService({ pool, clock, mailer, config, tokens, log });

  const app = express();
  app.set('trust proxy', parseTrustProxy(config.TRUST_PROXY));
  app.use(helmet());
  app.use(express.json({ limit: '10kb' }));
  app.use(
    buildRoutes({
      pool,
      keys,
      tokens,
      limiter: createRateLimiter({ pool, clock }),
      auth,
      admin: createAdminService({ pool, clock, auth }),
      realms: createRealmService({ pool, clock }),
    }),
  );
  app.use(notFoundHandler);
  app.use(errorHandler(log));
  return app;
}
