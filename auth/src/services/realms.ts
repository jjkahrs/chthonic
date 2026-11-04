import type { Pool } from 'mysql2/promise';
import type { Clock } from '../clock.js';
import { isDuplicateKey } from '../db/pool.js';
import { AppError } from '../errors.js';
import {
  deleteRealm,
  findRealm,
  insertRealm,
  listRealms,
  updateRealm,
  type Realm,
  type RealmInput,
} from '../repositories/realms.js';

const realmNotFound = () => new AppError(404, 'realm_not_found', 'Realm not found');
const nameTaken = () => new AppError(409, 'realm_name_taken', 'A realm with this name already exists');

async function uniqueName<T>(write: () => Promise<T>): Promise<T> {
  try {
    return await write();
  } catch (err) {
    throw isDuplicateKey(err) ? nameTaken() : err;
  }
}

/** What players see: the world admin endpoint is internal. */
export type PublicRealm = Omit<Realm, 'adminHost' | 'adminPort'>;

const toPublicRealm = ({ adminHost: _host, adminPort: _port, ...realm }: Realm): PublicRealm => realm;

export type RealmService = ReturnType<typeof createRealmService>;

export function createRealmService(deps: { pool: Pool; clock: Clock }) {
  const { pool, clock } = deps;

  return {
    list: (): Promise<Realm[]> => listRealms(pool),

    listPublic: async (): Promise<PublicRealm[]> => (await listRealms(pool)).map(toPublicRealm),

    async get(id: number): Promise<Realm> {
      const realm = await findRealm(pool, id);
      if (!realm) throw realmNotFound();
      return realm;
    },

    async create(input: RealmInput): Promise<Realm> {
      const id = await uniqueName(() => insertRealm(pool, input, clock.now()));
      return { id, ...input };
    },

    async update(id: number, input: RealmInput): Promise<Realm> {
      const found = await uniqueName(() => updateRealm(pool, id, input, clock.now()));
      if (!found) throw realmNotFound();
      return { id, ...input };
    },

    async remove(id: number): Promise<void> {
      if (!(await deleteRealm(pool, id))) throw realmNotFound();
    },
  };
}
