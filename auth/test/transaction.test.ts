import { describe, expect, it } from 'vitest';
import { withTransaction, type Db } from '../src/db/pool.js';
import { createAccount, useTestApp } from './helpers.js';

const ctx = useTestApp();

const lockAccount = (conn: Db, id: number) => conn.query('SELECT id FROM accounts WHERE id = ? FOR UPDATE', [id]);

describe('withTransaction', () => {
  it('retries a transaction InnoDB picked as the deadlock victim', async () => {
    const a = await createAccount(ctx, 'a@example.com');
    const b = await createAccount(ctx, 'b@example.com');
    let calls = 0;
    let firstLocks = 0;
    let bothLocked!: () => void;
    const barrier = new Promise<void>((resolve) => (bothLocked = resolve));

    // Each transaction locks one row, waits until the other holds its row, then locks the other row: a deadlock.
    const crossLock = (first: number, second: number) =>
      withTransaction(ctx.pool, async (conn) => {
        calls++;
        await lockAccount(conn, first);
        if (++firstLocks === 2) bothLocked();
        await barrier;
        await lockAccount(conn, second);
      });

    await Promise.all([crossLock(a, b), crossLock(b, a)]);
    expect(calls).toBe(3);
  });

  it('does not retry other errors', async () => {
    let calls = 0;
    const boom = new Error('boom');
    await expect(
      withTransaction(ctx.pool, async () => {
        calls++;
        throw boom;
      }),
    ).rejects.toBe(boom);
    expect(calls).toBe(1);
  });

  it('gives up after 3 deadlocks', async () => {
    let calls = 0;
    const deadlock = Object.assign(new Error('Deadlock found'), { code: 'ER_LOCK_DEADLOCK' });
    await expect(
      withTransaction(ctx.pool, async () => {
        calls++;
        throw deadlock;
      }),
    ).rejects.toBe(deadlock);
    expect(calls).toBe(3);
  });
});
