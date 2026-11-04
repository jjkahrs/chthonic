import type { ResultSetHeader, RowDataPacket } from 'mysql2/promise';
import type { Db } from '../db/pool.js';

export const REALM_STATUSES = ['online', 'offline', 'maintenance'] as const;

export interface RealmInput {
  name: string;
  host: string;
  port: number;
  /** Internal world admin endpoint; both set or both null. Admin-only, never shown to players. */
  adminHost: string | null;
  adminPort: number | null;
  region: string;
  status: (typeof REALM_STATUSES)[number];
  sortOrder: number;
}

export interface Realm extends RealmInput {
  id: number;
}

const COLUMNS = 'id, name, host, port, admin_host, admin_port, region, status, sort_order';
const toRealm = (r: RowDataPacket): Realm => ({
  id: r.id,
  name: r.name,
  host: r.host,
  port: r.port,
  adminHost: r.admin_host,
  adminPort: r.admin_port,
  region: r.region,
  status: r.status,
  sortOrder: r.sort_order,
});

export async function listRealms(db: Db): Promise<Realm[]> {
  const [rows] = await db.query<RowDataPacket[]>(`SELECT ${COLUMNS} FROM realms ORDER BY sort_order, id`);
  return rows.map(toRealm);
}

export async function findRealm(db: Db, id: number): Promise<Realm | undefined> {
  const [rows] = await db.query<RowDataPacket[]>(`SELECT ${COLUMNS} FROM realms WHERE id = ?`, [id]);
  return rows[0] && toRealm(rows[0]);
}

export async function insertRealm(db: Db, r: RealmInput, now: Date): Promise<number> {
  const [res] = await db.query<ResultSetHeader>(
    `INSERT INTO realms (name, host, port, admin_host, admin_port, region, status, sort_order, created_at, updated_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
    [r.name, r.host, r.port, r.adminHost, r.adminPort, r.region, r.status, r.sortOrder, now, now],
  );
  return res.insertId;
}

/** Returns false when no realm has this id. */
export async function updateRealm(db: Db, id: number, r: RealmInput, now: Date): Promise<boolean> {
  const [res] = await db.query<ResultSetHeader>(
    `UPDATE realms SET name = ?, host = ?, port = ?, admin_host = ?, admin_port = ?, region = ?, status = ?,
      sort_order = ?, updated_at = ? WHERE id = ?`,
    [r.name, r.host, r.port, r.adminHost, r.adminPort, r.region, r.status, r.sortOrder, now, id],
  );
  return res.affectedRows > 0;
}

/** Returns false when no realm has this id. */
export async function deleteRealm(db: Db, id: number): Promise<boolean> {
  const [res] = await db.query<ResultSetHeader>('DELETE FROM realms WHERE id = ?', [id]);
  return res.affectedRows > 0;
}
