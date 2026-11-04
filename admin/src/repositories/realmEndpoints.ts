import type { Pool, RowDataPacket } from 'mysql2/promise';

export interface RealmEndpoint {
  id: number;
  name: string;
  adminHost: string | null;
  adminPort: number | null;
}

export async function listRealmEndpoints(db: Pool): Promise<RealmEndpoint[]> {
  const [rows] = await db.query<RowDataPacket[]>(
    'SELECT realm_id, name, admin_host, admin_port FROM realm_endpoints ORDER BY realm_id',
  );
  return rows.map((r) => ({ id: r.realm_id, name: r.name, adminHost: r.admin_host, adminPort: r.admin_port }));
}

/** Replaces the whole copy in one transaction, so readers never see it half written. */
export async function replaceRealmEndpoints(db: Pool, realms: RealmEndpoint[], now: Date): Promise<void> {
  const conn = await db.getConnection();
  try {
    await conn.beginTransaction();
    await conn.query('DELETE FROM realm_endpoints');
    if (realms.length) {
      await conn.query('INSERT INTO realm_endpoints (realm_id, name, admin_host, admin_port, synced_at) VALUES ?', [
        realms.map((r) => [r.id, r.name, r.adminHost, r.adminPort, now]),
      ]);
    }
    await conn.commit();
  } catch (err) {
    await conn.rollback();
    throw err;
  } finally {
    conn.release();
  }
}
