import { createLocalJWKSet, jwtVerify, SignJWT } from 'jose';
import type { Clock } from '../clock.js';
import type { Config } from '../config.js';
import { unauthorized } from '../errors.js';
import type { SigningKeys } from '../keys.js';
import { ROLES, type Account, type Role } from '../repositories/accounts.js';

export interface AuthContext {
  accountId: number;
  email: string;
  role: Role;
}

export interface TokenService {
  signAccessToken(account: Pick<Account, 'id' | 'email' | 'role'>): Promise<{ token: string; expiresAt: Date }>;
  /** Throws 401 for a missing, expired, tampered or foreign token. */
  verifyAccessToken(token: string): Promise<AuthContext>;
}

export function createTokenService(deps: { config: Config; clock: Clock; keys: SigningKeys }): TokenService {
  const { config, clock, keys } = deps;
  // Verify through the published JWK Set, exactly as the World Server will.
  const jwks = createLocalJWKSet(keys.jwks);

  return {
    async signAccessToken(account) {
      const iat = Math.floor(clock.now().getTime() / 1000);
      const exp = iat + config.ACCESS_TOKEN_TTL_SECONDS;
      const token = await new SignJWT({ email: account.email, role: account.role })
        .setProtectedHeader({ alg: 'RS256', kid: keys.kid, typ: 'JWT' })
        .setSubject(String(account.id))
        .setIssuer(config.JWT_ISSUER)
        .setAudience(config.JWT_AUDIENCE)
        .setIssuedAt(iat)
        .setExpirationTime(exp)
        .sign(keys.privateKey);
      return { token, expiresAt: new Date(exp * 1000) };
    },

    async verifyAccessToken(token) {
      try {
        const { payload } = await jwtVerify(token, jwks, {
          issuer: config.JWT_ISSUER,
          audience: config.JWT_AUDIENCE,
          algorithms: ['RS256'],
          currentDate: clock.now(),
          requiredClaims: ['sub', 'exp', 'iat'],
        });
        const accountId = Number(payload.sub);
        const role = payload.role as Role;
        if (!Number.isSafeInteger(accountId) || typeof payload.email !== 'string' || !ROLES.includes(role)) {
          throw new Error('malformed claims');
        }
        return { accountId, email: payload.email, role };
      } catch {
        throw unauthorized();
      }
    },
  };
}
