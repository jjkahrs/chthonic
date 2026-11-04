import { createPrivateKey, createPublicKey, generateKeyPairSync, type KeyObject } from 'node:crypto';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { calculateJwkThumbprint, type JWK } from 'jose';
import type { Config } from './config.js';

export interface SigningKeys {
  kid: string;
  privateKey: KeyObject;
  /** Public JWK Set served at /.well-known/jwks.json. */
  jwks: { keys: JWK[] };
}

const DEV_KEY_FILE = 'jwt-private.pem';

export async function signingKeysFromPem(pem: string, kid?: string): Promise<SigningKeys> {
  const privateKey = createPrivateKey(pem);
  if (privateKey.asymmetricKeyType !== 'rsa') throw new Error('JWT private key must be an RSA key (RS256)');
  const publicJwk = createPublicKey(privateKey).export({ format: 'jwk' }) as JWK;
  const resolvedKid = kid ?? (await calculateJwkThumbprint(publicJwk));
  return {
    kid: resolvedKid,
    privateKey,
    jwks: { keys: [{ ...publicJwk, kid: resolvedKid, alg: 'RS256', use: 'sig' }] },
  };
}

/** Production keys come from env or a mounted file; otherwise a dev key is generated once and reused. */
export async function loadSigningKeys(config: Config, log: (msg: string) => void = console.log): Promise<SigningKeys> {
  // Env values often arrive with escaped newlines.
  if (config.JWT_PRIVATE_KEY) return signingKeysFromPem(config.JWT_PRIVATE_KEY.replace(/\\n/g, '\n'), config.JWT_KID);
  if (config.JWT_PRIVATE_KEY_FILE) {
    return signingKeysFromPem(await readFile(config.JWT_PRIVATE_KEY_FILE, 'utf8'), config.JWT_KID);
  }

  const file = path.resolve(config.KEYS_DIR, DEV_KEY_FILE);
  try {
    return await signingKeysFromPem(await readFile(file, 'utf8'), config.JWT_KID);
  } catch (err) {
    if ((err as NodeJS.ErrnoException).code !== 'ENOENT') throw err;
  }
  const { privateKey } = generateKeyPairSync('rsa', { modulusLength: 2048 });
  await mkdir(path.dirname(file), { recursive: true });
  await writeFile(file, privateKey.export({ format: 'pem', type: 'pkcs8' }), { mode: 0o600, flag: 'wx' });
  log(`Generated development JWT signing key at ${file}`);
  return signingKeysFromPem(await readFile(file, 'utf8'), config.JWT_KID);
}
