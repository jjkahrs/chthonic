import { Router, type RequestHandler } from 'express';
import type { Pool } from 'mysql2/promise';
import { AppError } from '../errors.js';
import type { SigningKeys } from '../keys.js';
import type { AdminService } from '../services/admin.js';
import type { AuthService } from '../services/auth.js';
import { RATE_LIMITS, type RateLimiter } from '../services/rateLimiter.js';
import type { RealmService } from '../services/realms.js';
import type { TokenService } from '../services/tokens.js';
import { adminHandlers } from './handlers/admin.js';
import { authHandlers } from './handlers/auth.js';
import { realmHandlers } from './handlers/realms.js';
import { noStore, rateLimit, requireAdmin, requireAuth, requireRole } from './middleware.js';

export interface RouteDeps {
  pool: Pool;
  keys: SigningKeys;
  tokens: TokenService;
  limiter: RateLimiter;
  auth: AuthService;
  admin: AdminService;
  realms: RealmService;
}

export function buildRoutes(deps: RouteDeps): Router {
  const auth = authHandlers(deps.auth);
  const admin = adminHandlers(deps.admin);
  const realms = realmHandlers(deps.realms);
  const authenticated = requireAuth(deps.tokens);
  const limit = (rule: (typeof RATE_LIMITS)[keyof typeof RATE_LIMITS]): RequestHandler => rateLimit(deps.limiter, rule);

  const router = Router();

  router.get('/health', async (_req, res) => {
    try {
      await deps.pool.query('SELECT 1');
    } catch {
      throw new AppError(503, 'db_unavailable', 'Database is unreachable');
    }
    res.json({ status: 'ok', db: 'ok' });
  });

  router.get('/.well-known/jwks.json', (_req, res) => {
    res.set('Cache-Control', 'public, max-age=300').json(deps.keys.jwks);
  });

  const v1 = Router();
  v1.use(noStore);

  v1.post('/auth/register', limit(RATE_LIMITS.register), auth.register);
  v1.post('/auth/verify-email', auth.verifyEmail);
  v1.post('/auth/resend-verification', limit(RATE_LIMITS.resendVerification), auth.resendVerification);
  v1.post('/auth/login', limit(RATE_LIMITS.login), auth.login);
  v1.post('/auth/refresh', auth.refresh);
  v1.post('/auth/logout', auth.logout);
  v1.post('/auth/forgot-password', limit(RATE_LIMITS.forgotPassword), auth.forgotPassword);
  v1.post('/auth/reset-password', auth.resetPassword);

  v1.get('/me', authenticated, auth.me);
  v1.get('/realms', authenticated, realms.listPublic);

  const adminRouter = Router();
  adminRouter.use(authenticated);
  // GMs may view accounts and ban/unban; the admin service limits them to player targets.
  const staff = requireRole('admin', 'gm');
  adminRouter.get('/accounts', staff, admin.list);
  adminRouter.get('/accounts/:id', staff, admin.get);
  adminRouter.post('/accounts/:id/ban', staff, admin.ban);
  adminRouter.delete('/accounts/:id/ban', staff, admin.unban);
  // Everything below is admin-only.
  adminRouter.use(requireAdmin);
  adminRouter.post('/accounts', admin.create);
  adminRouter.delete('/accounts/:id', admin.remove);
  adminRouter.post('/accounts/:id/verify-email', admin.verifyEmail);
  adminRouter.post('/accounts/:id/password-reset', admin.passwordReset);
  adminRouter.post('/accounts/:id/unlock', admin.unlock);
  adminRouter.post('/accounts/:id/revoke-sessions', admin.revokeSessions);
  adminRouter.put('/accounts/:id/role', admin.setRole);
  adminRouter.get('/realms', realms.list);
  adminRouter.post('/realms', realms.create);
  adminRouter.get('/realms/:id', realms.get);
  adminRouter.put('/realms/:id', realms.update);
  adminRouter.delete('/realms/:id', realms.remove);
  v1.use('/admin', adminRouter);

  router.use('/v1', v1);
  return router;
}
