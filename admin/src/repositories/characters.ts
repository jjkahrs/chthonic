import type { Pool, ResultSetHeader, RowDataPacket } from 'mysql2/promise';

// chthonic_world.characters, owned by the world server. The admin DB user may only SELECT and UPDATE it.

export interface CharacterSummary {
  id: number;
  realmId: number;
  accountId: number;
  name: string;
  mapId: number;
  createdAt: string;
  lastLoginAt: string | null;
  deletedAt: string | null;
}

export interface Character extends CharacterSummary {
  position: { x: number; y: number; z: number };
  heading: number;
  components: Record<string, unknown> | null;
  options: Record<string, unknown> | null;
}

export interface CharacterFilter {
  realmId?: number;
  accountId?: number;
  /** Letters only (validated), so it needs no LIKE escaping. */
  namePrefix?: string;
  includeDeleted: boolean;
  limit: number;
  offset: number;
}

export interface Placement {
  mapId: number;
  x: number;
  y: number;
  z: number;
  heading: number;
}

const iso = (d: Date | null) => d?.toISOString() ?? null;

const toSummary = (r: RowDataPacket): CharacterSummary => ({
  id: Number(r.id),
  realmId: r.realm_id,
  accountId: Number(r.account_id),
  name: r.name,
  mapId: r.map_id,
  createdAt: (r.created_at as Date).toISOString(),
  lastLoginAt: iso(r.last_login_at),
  deletedAt: iso(r.deleted_at),
});

const SUMMARY_COLUMNS = 'id, realm_id, account_id, name, map_id, created_at, last_login_at, deleted_at';

export async function searchCharacters(
  db: Pool,
  f: CharacterFilter,
): Promise<{ characters: CharacterSummary[]; total: number }> {
  const where: string[] = [];
  const args: unknown[] = [];
  const add = (sql: string, value: unknown) => {
    if (value === undefined) return;
    where.push(sql);
    args.push(value);
  };
  add('realm_id = ?', f.realmId);
  add('account_id = ?', f.accountId);
  add('name LIKE ?', f.namePrefix === undefined ? undefined : `${f.namePrefix}%`);
  if (!f.includeDeleted) where.push('deleted_at IS NULL');
  const clause = where.length ? `WHERE ${where.join(' AND ')}` : '';

  const [[count]] = await db.query<RowDataPacket[]>(`SELECT COUNT(*) AS total FROM characters ${clause}`, args);
  const [rows] = await db.query<RowDataPacket[]>(
    `SELECT ${SUMMARY_COLUMNS} FROM characters ${clause} ORDER BY id LIMIT ? OFFSET ?`,
    [...args, f.limit, f.offset],
  );
  return { characters: rows.map(toSummary), total: Number(count?.total ?? 0) };
}

export async function findCharacter(db: Pool, id: number): Promise<Character | undefined> {
  const [rows] = await db.query<RowDataPacket[]>(
    `SELECT ${SUMMARY_COLUMNS}, pos_x, pos_y, pos_z, heading, components, options FROM characters WHERE id = ?`,
    [id],
  );
  const r = rows[0];
  return (
    r && {
      ...toSummary(r),
      position: { x: r.pos_x, y: r.pos_y, z: r.pos_z },
      heading: r.heading,
      components: r.components,
      options: r.options,
    }
  );
}

async function update(db: Pool, id: number, set: string, args: unknown[]): Promise<void> {
  await db.query<ResultSetHeader>(`UPDATE characters SET ${set} WHERE id = ?`, [...args, id]);
}

/** Throws ER_DUP_ENTRY when the realm already has the name (deleted characters included, case-insensitive). */
export const renameCharacter = (db: Pool, id: number, name: string) => update(db, id, 'name = ?', [name]);

export const setCharacterDeletedAt = (db: Pool, id: number, deletedAt: Date | null) =>
  update(db, id, 'deleted_at = ?', [deletedAt]);

export const placeCharacter = (db: Pool, id: number, p: Placement) =>
  update(db, id, 'map_id = ?, pos_x = ?, pos_y = ?, pos_z = ?, heading = ?', [p.mapId, p.x, p.y, p.z, p.heading]);

export const setCharacterJson = (db: Pool, id: number, column: 'components' | 'options', value: object) =>
  update(db, id, `${column} = ?`, [JSON.stringify(value)]);

/** Soft-deletes every live character of the account on every realm; returns how many. */
export async function softDeleteAccountCharacters(db: Pool, accountId: number, now: Date): Promise<number> {
  const [res] = await db.query<ResultSetHeader>(
    'UPDATE characters SET deleted_at = ? WHERE account_id = ? AND deleted_at IS NULL',
    [now, accountId],
  );
  return res.affectedRows;
}
