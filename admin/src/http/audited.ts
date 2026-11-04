import type { Request, RequestHandler } from 'express';
import type { Pool } from 'mysql2/promise';
import type { Clock } from '../clock.js';
import { forbidden } from '../errors.js';
import { insertAudit } from '../repositories/audit.js';
import type { Role, SessionContext } from '../services/session.js';
import { sessionOf, toAppError } from './middleware.js';

/** What a mutating handler tells the audit log; targetId starts as :id. */
export interface AuditNote {
  targetId: string | null;
  before?: unknown;
  after?: unknown;
  forced?: boolean;
}

export interface Reply {
  status?: number;
  /** Omitted: 204. */
  body?: unknown;
}

export interface ActionContext extends SessionContext {
  note: AuditNote;
}

export type Action = (req: Request, ctx: ActionContext) => Promise<Reply>;

export type Audited = (action: string, targetType: string, roles: Role[], run: Action) => RequestHandler;

/**
 * Wraps a mutating route so it writes exactly one admin_audit row, on success and on failure (including a role
 * refusal), before the response goes out. Runs after requireSession, so the actor is always known.
 */
export function auditor(deps: { pool: Pool; clock: Clock }): Audited {
  return (action, targetType, roles, run) => async (req, res) => {
    const { caller, token } = sessionOf(res);
    const note: AuditNote = { targetId: typeof req.params.id === 'string' ? req.params.id : null };
    const record = (result: string) =>
      insertAudit(deps.pool, {
        actorAccountId: caller.id,
        actorEmail: caller.email,
        actorRole: caller.role,
        action,
        targetType,
        targetId: note.targetId,
        before: note.before,
        after: note.after,
        forced: note.forced ?? false,
        result,
        createdAt: deps.clock.now(),
      });

    let reply: Reply;
    try {
      if (!roles.includes(caller.role)) throw forbidden();
      reply = await run(req, { caller, token, note });
    } catch (err) {
      await record(toAppError(err)?.code ?? 'internal_error');
      throw err;
    }
    await record('ok');
    if (reply.body === undefined) res.status(reply.status ?? 204).end();
    else res.status(reply.status ?? 200).json(reply.body);
  };
}
