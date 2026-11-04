import { Router } from 'express';
import type { AuthClient, AuthRequest } from '../../authClient.js';
import { AppError } from '../../errors.js';
import type { CharacterService } from '../../services/characters.js';
import type { RealmService } from '../../services/realms.js';
import type { SessionContext } from '../../services/session.js';
import type { Audited } from '../audited.js';
import { parse, requireRole, sessionOf } from '../middleware.js';
import { accountQuery, banBody, createAccountBody, idParams, roleBody } from '../schemas.js';

interface Account {
  id: number;
  role: string;
}

/** Proxies auth's /v1/admin/accounts with the caller's token; auth re-checks every permission. */
export function accountRoutes(deps: {
  auth: AuthClient;
  realms: RealmService;
  characters: CharacterService;
  audited: Audited;
}): Router {
  const { auth, realms, characters, audited } = deps;
  const proxy = <T>(ctx: SessionContext, path: string, opts: AuthRequest = {}) =>
    auth.request<T>(`/v1/admin/accounts${path}`, { ...opts, token: ctx.token });
  const r = Router();

  r.get('/', requireRole('admin', 'gm'), async (req, res) => {
    res.json(await proxy(sessionOf(res), '', { query: parse(accountQuery, req.query) }));
  });

  r.get('/:id', requireRole('admin', 'gm'), async (req, res) => {
    res.json(await proxy(sessionOf(res), `/${parse(idParams, req.params).id}`));
  });

  r.post(
    '/',
    audited('account.create', 'account', ['admin'], async (req, ctx) => {
      const body = parse(createAccountBody, req.body);
      const account = await proxy<Account>(ctx, '', { method: 'POST', body });
      ctx.note.targetId = String(account.id);
      ctx.note.after = { id: account.id, email: body.email, role: body.role }; // never the password
      return { status: 201, body: account };
    }),
  );

  // Kick everywhere, then delete in auth, then soft-delete the characters. Unreachable realms don't block the delete.
  r.delete(
    '/:id',
    audited('account.delete', 'account', ['admin'], async (req, ctx) => {
      const { id } = parse(idParams, req.params);
      if (id === ctx.caller.id) {
        throw new AppError(400, 'cannot_modify_self', 'Admins cannot ban, delete or change the role of their own account');
      }
      ctx.note.before = await proxy<Account>(ctx, `/${id}`);
      const kicks = await realms.kickEverywhere(ctx, id, 'account deleted');
      await proxy(ctx, `/${id}`, { method: 'DELETE' });
      const charactersDeleted = await characters.softDeleteAccount(id);
      ctx.note.after = { kicks, charactersDeleted };
      return { body: { kicks, charactersDeleted } };
    }),
  );

  r.post(
    '/:id/ban',
    audited('account.ban', 'account', ['admin', 'gm'], async (req, ctx) => {
      const { id } = parse(idParams, req.params);
      const body = parse(banBody, req.body);
      const ban = await proxy(ctx, `/${id}/ban`, { method: 'POST', body });
      const kicks = await realms.kickEverywhere(ctx, id, 'banned');
      ctx.note.after = { ban, kicks };
      return { status: 201, body: { ban, kicks } };
    }),
  );

  r.delete(
    '/:id/ban',
    audited('account.unban', 'account', ['admin', 'gm'], async (req, ctx) => {
      await proxy(ctx, `/${parse(idParams, req.params).id}/ban`, { method: 'DELETE' });
      return {};
    }),
  );

  r.put(
    '/:id/role',
    audited('account.set_role', 'account', ['admin'], async (req, ctx) => {
      const { id } = parse(idParams, req.params);
      const { role } = parse(roleBody, req.body);
      const before = await proxy<Account>(ctx, `/${id}`);
      ctx.note.before = { role: before.role };
      const account = await proxy(ctx, `/${id}/role`, { method: 'PUT', body: { role } });
      // Any role change ends the account's game sessions, so a demoted GM loses GM powers in the world at once.
      const kicks = role === before.role ? null : await realms.kickEverywhere(ctx, id, 'role changed');
      ctx.note.after = { role, kicks };
      return { body: { account, kicks } };
    }),
  );

  for (const action of ['verify-email', 'password-reset', 'unlock', 'revoke-sessions']) {
    r.post(
      `/:id/${action}`,
      audited(`account.${action.replace('-', '_')}`, 'account', ['admin'], async (req, ctx) => {
        await proxy(ctx, `/${parse(idParams, req.params).id}/${action}`, { method: 'POST' });
        return {};
      }),
    );
  }

  return r;
}
