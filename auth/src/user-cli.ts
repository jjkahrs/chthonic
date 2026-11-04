import { createInterface } from 'node:readline/promises';
import { Writable } from 'node:stream';
import { pathToFileURL } from 'node:url';
import { parseArgs } from 'node:util';
import type { z } from 'zod';
import { systemClock, type Clock } from './clock.js';
import { loadConfig } from './config.js';
import { createPool, isDuplicateKey, type Db } from './db/pool.js';
import { createAccountBody } from './http/schemas.js';
import { findAccountByEmail, insertAccount, ROLES, setRole } from './repositories/accounts.js';
import { hashPassword } from './services/crypto.js';

const USAGE = `Usage:
  npm run user -- create <email> <role> [--password <password>]
  npm run user -- set-role <email> <role>
Roles: ${ROLES.join(', ')}. Without --password, create prompts for it.`;

class UsageError extends Error {}

/** Runs one command and returns the line to print; throws on bad input or a failed command. */
export async function runUserCommand(
  db: Db,
  argv: string[],
  readPassword: () => Promise<string>,
  clock: Clock = systemClock,
): Promise<string> {
  let parsed;
  try {
    parsed = parseArgs({ args: argv, allowPositionals: true, options: { password: { type: 'string' } } });
  } catch (err) {
    throw new UsageError((err as Error).message);
  }
  const [command, rawEmail, rawRole, ...extra] = parsed.positionals;
  if (!rawEmail || !rawRole || extra.length) throw new UsageError('Expected a command, an email and a role');

  if (command === 'create') {
    const input = parse(createAccountBody, {
      email: rawEmail,
      role: rawRole,
      password: parsed.values.password ?? (await readPassword()),
    });
    const now = clock.now();
    try {
      const id = await insertAccount(db, {
        email: input.email,
        passwordHash: await hashPassword(input.password),
        role: input.role,
        emailVerifiedAt: now,
        createdAt: now,
      });
      return `Created ${input.role} account ${input.email} (id ${id})`;
    } catch (err) {
      if (isDuplicateKey(err)) {
        throw new Error(`Account ${input.email} already exists; use set-role to change its role`, { cause: err });
      }
      throw err;
    }
  }

  if (command === 'set-role') {
    if (parsed.values.password !== undefined) throw new UsageError('set-role does not take --password');
    const input = parse(createAccountBody.pick({ email: true, role: true }), { email: rawEmail, role: rawRole });
    const account = await findAccountByEmail(db, input.email);
    if (!account) throw new Error(`No account with email ${input.email}`);
    await setRole(db, account.id, input.role);
    return `Changed ${input.email} (id ${account.id}) from ${account.role} to ${input.role}`;
  }

  throw new UsageError(`Unknown command: ${command}`);
}

function parse<T extends z.ZodType>(schema: T, value: unknown): z.output<T> {
  const result = schema.safeParse(value);
  if (result.success) return result.data;
  throw new Error(result.error.issues.map((i) => `${i.path.join('.')}: ${i.message}`).join('\n'));
}

/** Reads one line from stdin without echoing it, so the password stays off screen and out of shell history. */
async function promptPassword(): Promise<string> {
  process.stdout.write('Password: ');
  const muted = new Writable({ write: (_chunk, _enc, done) => done() });
  const rl = createInterface({ input: process.stdin, output: muted, terminal: true });
  // ponytail: no confirm prompt; a mistyped password is fixed with the reset-password flow.
  const password = await rl.question('');
  rl.close();
  process.stdout.write('\n');
  return password;
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? '').href) {
  try {
    const pool = createPool(loadConfig());
    try {
      console.log(await runUserCommand(pool, process.argv.slice(2), promptPassword));
    } finally {
      await pool.end();
    }
  } catch (err) {
    console.error((err as Error).message);
    if (err instanceof UsageError) console.error(USAGE);
    process.exit(1);
  }
}
