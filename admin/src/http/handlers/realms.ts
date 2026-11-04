import { Router } from 'express';
import type { AuthClient } from '../../authClient.js';
import type { RealmService } from '../../services/realms.js';
import type { SessionContext } from '../../services/session.js';
import type { Audited } from '../audited.js';
import { parse, requireRole, sessionOf } from '../middleware.js';
import { idParams, kickBody, realmBody } from '../schemas.js';

/** Realm CRUD goes to auth (admin only); online, instances and kick talk to each realm's world admin port. */
export function realmRoutes(deps: { auth: AuthClient; realms: RealmService; audited: Audited }): Router {
  const { auth, realms, audited } = deps;
  const proxy = <T>(ctx: SessionContext, path: string, method: 'GET' | 'POST' | 'PUT' | 'DELETE' = 'GET', body?: unknown) =>
    auth.request<T>(`/v1/admin/realms${path}`, { method, body, token: ctx.token });
  const r = Router();

  // Admins get auth's full realm view; GMs (for the Online page) only id, name and reachable.
  r.get('/', requireRole('admin', 'gm'), async (_req, res) => {
    const ctx = sessionOf(res);
    const withStatus = await realms.listWithStatus(await realms.endpoints(ctx));
    res.json({
      realms:
        ctx.caller.role === 'admin' ? withStatus : withStatus.map(({ id, name, reachable }) => ({ id, name, reachable })),
    });
  });

  r.get('/:id', requireRole('admin'), async (req, res) => {
    res.json(await proxy(sessionOf(res), `/${parse(idParams, req.params).id}`));
  });

  r.get('/:id/online', requireRole('admin', 'gm'), async (req, res) => {
    res.json({ online: await realms.online(sessionOf(res), parse(idParams, req.params).id) });
  });

  r.get('/:id/instances', requireRole('admin', 'gm'), async (req, res) => {
    res.json({ instances: await realms.instances(sessionOf(res), parse(idParams, req.params).id) });
  });

  r.post(
    '/:id/kick',
    audited('realm.kick', 'realm', ['admin', 'gm'], async (req, ctx) => {
      const { id } = parse(idParams, req.params);
      const { reason, ...target } = parse(kickBody, req.body);
      const kicked = await realms.kick(ctx, id, target, reason);
      ctx.note.after = { ...target, reason, kicked };
      return { body: { kicked } };
    }),
  );

  // Every write re-reads the list so the endpoint copy GMs use stays current.
  r.post(
    '/',
    audited('realm.create', 'realm', ['admin'], async (req, ctx) => {
      const realm = await proxy<{ id: number }>(ctx, '', 'POST', parse(realmBody, req.body));
      await realms.list(ctx.token);
      ctx.note.targetId = String(realm.id);
      ctx.note.after = realm;
      return { status: 201, body: realm };
    }),
  );

  r.put(
    '/:id',
    audited('realm.update', 'realm', ['admin'], async (req, ctx) => {
      const { id } = parse(idParams, req.params);
      const body = parse(realmBody, req.body);
      ctx.note.before = await proxy(ctx, `/${id}`);
      const realm = await proxy(ctx, `/${id}`, 'PUT', body);
      await realms.list(ctx.token);
      ctx.note.after = realm;
      return { body: realm };
    }),
  );

  r.delete(
    '/:id',
    audited('realm.delete', 'realm', ['admin'], async (req, ctx) => {
      const { id } = parse(idParams, req.params);
      ctx.note.before = await proxy(ctx, `/${id}`);
      await proxy(ctx, `/${id}`, 'DELETE');
      await realms.list(ctx.token);
      return {};
    }),
  );

  return r;
}
