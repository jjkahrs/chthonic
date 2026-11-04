import mysql, { type Pool, type PoolOptions } from 'mysql2/promise';
import type { Config } from '../config.js';

export function adminDbOptions(config: Config): PoolOptions {
  return dbOptions(config, config.ADMIN_DB_USER, config.ADMIN_DB_PASSWORD, config.ADMIN_DB_NAME);
}

export function worldDbOptions(config: Config): PoolOptions {
  return dbOptions(config, config.WORLD_DB_USER, config.WORLD_DB_PASSWORD, config.WORLD_DB_NAME);
}

function dbOptions(config: Config, user: string, password: string, database: string): PoolOptions {
  // DATETIME columns hold UTC; read and write JS Dates as UTC.
  return { host: config.DB_HOST, port: config.DB_PORT, user, password, database, timezone: 'Z' };
}

export function createPool(options: PoolOptions): Pool {
  return mysql.createPool({ ...options, connectionLimit: 10 });
}

export function isDuplicateKey(err: unknown): boolean {
  return (err as { code?: string }).code === 'ER_DUP_ENTRY';
}
