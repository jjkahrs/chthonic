import type { Pool, ResultSetHeader, RowDataPacket } from 'mysql2/promise';

export interface AuditInput {
  actorAccountId: number;
  actorEmail: string;
  actorRole: string;
  action: string;
  targetType: string;
  targetId: string | null;
  before: unknown;
  after: unknown;
  forced: boolean;
  /** 'ok' or the error code the request failed with. */
  result: string;
  createdAt: Date;
}

export interface AuditRow extends Omit<AuditInput, 'createdAt'> {
  id: number;
  createdAt: string;
}

export interface AuditFilter {
  actorId?: number;
  action?: string;
  targetType?: string;
  targetId?: string;
  from?: Date;
  to?: Date;
  limit: number;
  offset: number;
}

const json = (v: unknown) => (v === undefined || v === null ? null : JSON.stringify(v));

export async function insertAudit(db: Pool, a: AuditInput): Promise<number> {
  const [res] = await db.query<ResultSetHeader>(
    `INSERT INTO admin_audit (actor_account_id, actor_email, actor_role, action, target_type, target_id, \`before\`,
      \`after\`, forced, result, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
    [
      a.actorAccountId,
      a.actorEmail,
      a.actorRole,
      a.action,
      a.targetType,
      a.targetId,
      json(a.before),
      json(a.after),
      a.forced,
      a.result,
      a.createdAt,
    ],
  );
  return res.insertId;
}

/** Newest first. from is inclusive, to exclusive. */
export async function searchAudit(db: Pool, f: AuditFilter): Promise<{ entries: AuditRow[]; total: number }> {
  const where: string[] = [];
  const args: unknown[] = [];
  const add = (sql: string, value: unknown) => {
    if (value === undefined) return;
    where.push(sql);
    args.push(value);
  };
  add('actor_account_id = ?', f.actorId);
  add('action = ?', f.action);
  add('target_type = ?', f.targetType);
  add('target_id = ?', f.targetId);
  add('created_at >= ?', f.from);
  add('created_at < ?', f.to);
  const clause = where.length ? `WHERE ${where.join(' AND ')}` : '';

  const [[count]] = await db.query<RowDataPacket[]>(`SELECT COUNT(*) AS total FROM admin_audit ${clause}`, args);
  const [rows] = await db.query<RowDataPacket[]>(
    `SELECT * FROM admin_audit ${clause} ORDER BY id DESC LIMIT ? OFFSET ?`,
    [...args, f.limit, f.offset],
  );
  return {
    total: Number(count?.total ?? 0),
    entries: rows.map((r) => ({
      id: r.id,
      actorAccountId: r.actor_account_id,
      actorEmail: r.actor_email,
      actorRole: r.actor_role,
      action: r.action,
      targetType: r.target_type,
      targetId: r.target_id,
      before: r.before,
      after: r.after,
      forced: r.forced === 1,
      result: r.result,
      createdAt: (r.created_at as Date).toISOString(),
    })),
  };
}
