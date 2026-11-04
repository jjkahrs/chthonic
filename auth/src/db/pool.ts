import { setTimeout as sleep } from 'node:timers/promises';
import mysql,{ type Connection, type Pool, type PoolOptions } from 'mysql2/promise';
import type { Config } from '../config.js';

/** Anything that can run a query: the pool, or a connection inside a transaction. */
export type Db = Connection;

export function connectionOptions(config: Config): PoolOptions {
  return {
    host: config.DB_HOST,
    port: config.DB_PORT,
    user: config.DB_USER,
    password: config.DB_PASSWORD,
    database: config.DB_NAME,
    // DATETIME columns hold UTC; read and write JS Dates as UTC.
    timezone: 'Z',
  };
}

export function createPool(config: Config): Pool {
  return mysql.createPool({ ...connectionOptions(config), connectionLimit: 10 });
}

const MAX_TRANSACTION_ATTEMPTS = 3;

/**
 * Runs work in a transaction, rerunning it from the start if InnoDB picks it as a deadlock victim.
 * work may therefore run more than once: no side effects (mail, logs) before its last query.
 */
export async function withTransaction<T>(pool: Pool, work: (conn: Db) => Promise<T>): Promise<T> {
  for (let attempt = 1; ; attempt++) {
    const conn = await pool.getConnection();
    try {
      await conn.beginTransaction();
      const result = await work(conn);
      await conn.commit();
      return result;
    } catch (err) {
      await conn.rollback();
      // ponytail: ER_LOCK_WAIT_TIMEOUT is not retried; it already waited innodb_lock_wait_timeout (50s default).
      if (!isDeadlock(err) || attempt === MAX_TRANSACTION_ATTEMPTS) throw err;
    } finally {
      conn.release();
    }
    // Random backoff so two victims don't collide again in lockstep.
    await sleep(Math.random() * 20 * attempt);
  }
}

function isDeadlock(err: unknown): boolean {
  return (err as { code?: string }).code === 'ER_LOCK_DEADLOCK';
}

export function isDuplicateKey(err: unknown): boolean {
  return (err as { code?: string }).code === 'ER_DUP_ENTRY';
}
