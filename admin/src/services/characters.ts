import type { Pool } from 'mysql2/promise';
import type { Clock } from '../clock.js';
import { isDuplicateKey } from '../db/pool.js';
import { AppError, forbidden } from '../errors.js';
import {
  findCharacter,
  placeCharacter,
  renameCharacter,
  searchCharacters,
  setCharacterDeletedAt,
  setCharacterJson,
  softDeleteAccountCharacters,
  type Character,
  type CharacterFilter,
  type Placement,
} from '../repositories/characters.js';
import type { RealmService } from './realms.js';
import type { SessionContext } from './session.js';

/** A guarded write's outcome, for the response and the audit log. */
export interface Change {
  before: Character;
  after: Character;
  /** An admin's force skipped the online check because the realm could not be asked. */
  forced: boolean;
}

export type CharacterService = ReturnType<typeof createCharacterService>;

export function createCharacterService(deps: { pool: Pool; realms: RealmService; clock: Clock }) {
  const { pool, realms, clock } = deps;

  async function get(id: number): Promise<Character> {
    const character = await findCharacter(pool, id);
    if (!character) throw new AppError(404, 'character_not_found', 'Character not found');
    return character;
  }

  /**
   * Writes only while the owning account is offline on the character's realm, so the world server can't overwrite
   * the change when it next saves. If the realm can't be asked, only an admin's force goes ahead.
   */
  async function guarded(ctx: SessionContext, id: number, force: boolean, write: () => Promise<void>): Promise<Change> {
    if (force && ctx.caller.role !== 'admin') throw forbidden('Only admins can force a character write');
    const before = await get(id);
    let forced = false;
    try {
      if (await realms.isAccountOnline(ctx, before.realmId, before.accountId)) {
        throw new AppError(409, 'character_online', 'The character’s account is online; kick it first', {
          realmId: before.realmId,
          accountId: before.accountId,
        });
      }
    } catch (err) {
      if (!(force && err instanceof AppError && err.code === 'realm_unreachable')) throw err;
      forced = true;
    }
    // ponytail: the account could log in between the check and the write; the window is a few milliseconds.
    await write();
    return { before, after: await get(id), forced };
  }

  return {
    search: (filter: CharacterFilter) => searchCharacters(pool, filter),
    get,

    rename: (ctx: SessionContext, id: number, name: string, force: boolean) =>
      guarded(ctx, id, force, async () => {
        try {
          await renameCharacter(pool, id, name);
        } catch (err) {
          if (isDuplicateKey(err)) throw new AppError(409, 'name_taken', 'The realm already has a character with this name');
          throw err;
        }
      }),

    remove: (ctx: SessionContext, id: number, force: boolean) =>
      guarded(ctx, id, force, () => setCharacterDeletedAt(pool, id, clock.now())),

    restore: (ctx: SessionContext, id: number, force: boolean) =>
      guarded(ctx, id, force, () => setCharacterDeletedAt(pool, id, null)),

    move: (ctx: SessionContext, id: number, placement: Placement, force: boolean) =>
      guarded(ctx, id, force, () => placeCharacter(pool, id, placement)),

    setJson: (ctx: SessionContext, id: number, column: 'components' | 'options', value: object, force: boolean) =>
      guarded(ctx, id, force, () => setCharacterJson(pool, id, column, value)),

    softDeleteAccount: (accountId: number) => softDeleteAccountCharacters(pool, accountId, clock.now()),
  };
}
