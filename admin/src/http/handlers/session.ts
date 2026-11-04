import { Router, type RequestHandler } from 'express';
import type { Logger } from '../../logger.js';
import type { RealmService } from '../../services/realms.js';
import type { SessionService } from '../../services/session.js';
import { COOKIES, parse, readCookies, sessionOf, type CookieWriter } from '../middleware.js';
import { loginBody } from '../schemas.js';

/** BFF session: auth's tokens live only in httpOnly cookies. Login and logout don't need a valid session. */
export function sessionRoutes(deps: {
  sessions: SessionService;
  cookies: CookieWriter;
  realms: RealmService;
  log: Logger;
  requireSession: RequestHandler;
}): Router {
  const { sessions, cookies, realms, log } = deps;
  const r = Router();

  r.get('/', deps.requireSession, (_req, res) => {
    res.json(sessionOf(res).caller);
  });

  r.post('/login', async (req, res) => {
    const { email, password } = parse(loginBody, req.body);
    const { caller, session } = await sessions.login(email, password);
    cookies.setSession(res, session);
    cookies.setCsrf(res); // a new token per session
    // Refresh the realm endpoint copy GMs depend on; not worth failing a login over.
    if (caller.role === 'admin') {
      await realms.list(session.accessToken).catch((err: Error) => log.warn(`Realm sync failed: ${err.message}`));
    }
    res.json(caller);
  });

  r.post('/logout', async (req, res) => {
    const refresh = readCookies(req)[COOKIES.refresh];
    // Already-invalid tokens are fine: the cookies go either way.
    if (refresh) await sessions.logout(refresh).catch(() => {});
    cookies.clearSession(res);
    res.status(204).end();
  });

  return r;
}
