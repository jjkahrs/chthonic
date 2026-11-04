import { Router, type Request } from 'express';
import type { CharacterService, Change } from '../../services/characters.js';
import type { SessionContext } from '../../services/session.js';
import type { Audited } from '../audited.js';
import { parse, requireRole } from '../middleware.js';
import { characterQuery, componentsBody, forceBody, idParams, moveBody, optionsBody, renameBody } from '../schemas.js';

/** Characters in chthonic_world. GMs may do everything here except force. */
export function characterRoutes(deps: { characters: CharacterService; audited: Audited }): Router {
  const { characters, audited } = deps;
  const r = Router();
  const staff = requireRole('admin', 'gm');

  r.get('/', staff, async (req, res) => {
    const { name, ...filter } = parse(characterQuery, req.query);
    res.json({ ...(await characters.search({ ...filter, namePrefix: name })), limit: filter.limit, offset: filter.offset });
  });

  r.get('/:id', staff, async (req, res) => {
    res.json(await characters.get(parse(idParams, req.params).id));
  });

  /** A guarded write: audits before/after and whether force was used; answers the character after the change. */
  const write = (action: string, run: (req: Request, ctx: SessionContext, id: number) => Promise<Change>) =>
    audited(action, 'character', ['admin', 'gm'], async (req, ctx) => {
      const change = await run(req, ctx, parse(idParams, req.params).id);
      Object.assign(ctx.note, change);
      return { body: change.after };
    });

  r.put(
    '/:id/name',
    write('character.rename', (req, ctx, id) => {
      const { name, force } = parse(renameBody, req.body);
      return characters.rename(ctx, id, name, force);
    }),
  );
  r.put(
    '/:id/position',
    write('character.move', (req, ctx, id) => {
      const { force, ...placement } = parse(moveBody, req.body);
      return characters.move(ctx, id, placement, force);
    }),
  );
  r.put(
    '/:id/components',
    write('character.edit_components', (req, ctx, id) => {
      const { components, force } = parse(componentsBody, req.body);
      return characters.setJson(ctx, id, 'components', components, force);
    }),
  );
  r.put(
    '/:id/options',
    write('character.edit_options', (req, ctx, id) => {
      const { options, force } = parse(optionsBody, req.body);
      return characters.setJson(ctx, id, 'options', options, force);
    }),
  );
  r.delete(
    '/:id',
    write('character.delete', (req, ctx, id) => characters.remove(ctx, id, parse(forceBody, req.body).force)),
  );
  r.post(
    '/:id/restore',
    write('character.restore', (req, ctx, id) => characters.restore(ctx, id, parse(forceBody, req.body).force)),
  );

  return r;
}
