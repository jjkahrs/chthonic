import path from 'node:path';
import { fileURLToPath } from 'node:url';
import express, { Router, type Express } from 'express';
import helmet from 'helmet';
import type { Pool } from 'mysql2/promise';
import type { AuthClient } from '../authClient.js';
import type { Clock } from '../clock.js';
import { parseTrustProxy, type Config } from '../config.js';
import { AppError } from '../errors.js';
import type { Logger } from '../logger.js';
import { searchAudit } from '../repositories/audit.js';
import { createCharacterService } from '../services/characters.js';
import { createRealmService } from '../services/realms.js';
import { createSessionService } from '../services/session.js';
import type { WorldAdminClient } from '../worldAdmin.js';
import { auditor } from './audited.js';
import { accountRoutes } from './handlers/accounts.js';
import { characterRoutes } from './handlers/characters.js';
import { realmRoutes } from './handlers/realms.js';
import { sessionRoutes } from './handlers/session.js';
import {
  cookieWriter,
  csrf,
  errorHandler,
  noStore,
  notFoundHandler,
  parse,
  requireRole,
  requireSession,
} from './middleware.js';
import { auditQuery } from './schemas.js';

// Resolves to admin/web/dist from both src/http and dist/http.
const DEFAULT_STATIC_DIR = fileURLToPath(new URL('../../web/dist', import.meta.url));

export interface AppDeps {
  config: Config;
  /** chthonic_admin */
  adminPool: Pool;
  /** chthonic_world, characters only */
  worldPool: Pool;
  clock: Clock;
  auth: AuthClient;
  world: WorldAdminClient;
  log: Logger;
}

/** Wires services to HTTP. Every external dependency is passed in, so tests can swap any of them. */
export function createApp(deps: AppDeps): Express {
  const { config, adminPool, worldPool, clock, auth, world, log } = deps;
  const sessions = createSessionService({ auth, clock, issuer: config.JWT_ISSUER, audience: config.JWT_AUDIENCE });
  const realms = createRealmService({ auth, world, pool: adminPool, clock });
  const characters = createCharacterService({ pool: worldPool, realms, clock });
  const cookies = cookieWriter(config.COOKIE_SECURE);
  const audited = auditor({ pool: adminPool, clock });
  const session = requireSession(sessions, cookies);

  const api = Router();
  api.use(noStore, express.json({ limit: '100kb' }), csrf(cookies));
  api.use('/session', sessionRoutes({ sessions, cookies, realms, log, requireSession: session }));
  api.use(session);
  api.use('/accounts', accountRoutes({ auth, realms, characters, audited }));
  api.use('/characters', characterRoutes({ characters, audited }));
  api.use('/realms', realmRoutes({ auth, realms, audited }));
  api.get('/audit', requireRole('admin'), async (req, res) => {
    const filter = parse(auditQuery, req.query);
    res.json({ ...(await searchAudit(adminPool, filter)), limit: filter.limit, offset: filter.offset });
  });
  api.use(notFoundHandler);

  const app = express();
  app.set('trust proxy', parseTrustProxy(config.TRUST_PROXY));
  app.use(helmet());
  app.get('/health', async (_req, res) => {
    try {
      await Promise.all([adminPool.query('SELECT 1'), worldPool.query('SELECT 1')]);
    } catch {
      throw new AppError(503, 'db_unavailable', 'Database is unreachable');
    }
    res.json({ status: 'ok', db: 'ok' });
  });
  app.use('/api', api);

  // The SPA: real files first, then index.html for any other GET so client-side routes survive a reload.
  const staticDir = path.resolve(config.STATIC_DIR ?? DEFAULT_STATIC_DIR);
  app.use(express.static(staticDir, { index: false }));
  app.get('/{*path}', (_req, res, next) => {
    res.sendFile(path.join(staticDir, 'index.html'), (err) => err && next());
  });
  app.use(notFoundHandler);
  app.use(errorHandler(log));
  return app;
}
