import type { Request, Response } from 'express';
import type { AdminService } from '../../services/admin.js';
import { authOf, parse } from '../middleware.js';
import { accountQuery, banBody, createAccountBody, idParams, roleBody } from '../schemas.js';

export function adminHandlers(admin: AdminService) {
  /** Runs an id-only account action and answers 204. */
  const accountAction = (action: (id: number) => Promise<void>) => async (req: Request, res: Response) => {
    await action(parse(idParams, req.params).id);
    res.status(204).end();
  };

  return {
    async list(req: Request, res: Response) {
      res.json(await admin.list(authOf(res), parse(accountQuery, req.query)));
    },

    async get(req: Request, res: Response) {
      res.json(await admin.get(authOf(res), parse(idParams, req.params).id));
    },

    async create(req: Request, res: Response) {
      res.status(201).json(await admin.create(parse(createAccountBody, req.body)));
    },

    async remove(req: Request, res: Response) {
      await admin.remove(authOf(res).accountId, parse(idParams, req.params).id);
      res.status(204).end();
    },

    verifyEmail: accountAction(admin.verifyEmail),
    passwordReset: accountAction(admin.sendPasswordReset),
    unlock: accountAction(admin.unlock),
    revokeSessions: accountAction(admin.revokeSessions),

    async ban(req: Request, res: Response) {
      const { id } = parse(idParams, req.params);
      res.status(201).json(await admin.ban(authOf(res), id, parse(banBody, req.body)));
    },

    async unban(req: Request, res: Response) {
      await admin.unban(authOf(res), parse(idParams, req.params).id);
      res.status(204).end();
    },

    async setRole(req: Request, res: Response) {
      const { id } = parse(idParams, req.params);
      res.json(await admin.setRole(authOf(res).accountId, id, parse(roleBody, req.body).role));
    },
  };
}
