import type { Request, Response } from 'express';
import type { RealmService } from '../../services/realms.js';
import { parse } from '../middleware.js';
import { idParams, realmBody } from '../schemas.js';

export function realmHandlers(realms: RealmService) {
  return {
    async list(_req: Request, res: Response) {
      res.json({ realms: await realms.list() });
    },

    async listPublic(_req: Request, res: Response) {
      res.json({ realms: await realms.listPublic() });
    },

    async get(req: Request, res: Response) {
      res.json(await realms.get(parse(idParams, req.params).id));
    },

    async create(req: Request, res: Response) {
      res.status(201).json(await realms.create(parse(realmBody, req.body)));
    },

    async update(req: Request, res: Response) {
      const { id } = parse(idParams, req.params);
      res.json(await realms.update(id, parse(realmBody, req.body)));
    },

    async remove(req: Request, res: Response) {
      await realms.remove(parse(idParams, req.params).id);
      res.status(204).end();
    },
  };
}
