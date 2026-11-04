import { createHash, randomBytes } from 'node:crypto';
import bcrypt from 'bcrypt';

const BCRYPT_COST = 12;

/** 256-bit random value, URL-safe; used for refresh, verify and reset tokens. */
export const randomToken = (): string => randomBytes(32).toString('base64url');

/** Tokens are stored only as SHA-256 hex digests. */
export const sha256Hex = (value: string): string => createHash('sha256').update(value).digest('hex');

// bcrypt ignores everything past 72 bytes. Pre-hashing to a fixed 44-char string
// makes every character of a 128-char password count.
const prehash = (password: string) => createHash('sha256').update(password, 'utf8').digest('base64');

export const hashPassword = (password: string): Promise<string> => bcrypt.hash(prehash(password), BCRYPT_COST);

export const verifyPassword = (password: string, hash: string): Promise<boolean> =>
  bcrypt.compare(prehash(password), hash);

let dummyHash: Promise<string> | undefined;
/** Burns one bcrypt compare for unknown emails, so they take as long as a wrong password. */
export async function verifyAgainstDummy(password: string): Promise<void> {
  dummyHash ??= hashPassword(randomToken());
  await verifyPassword(password, await dummyHash);
}
