import type { Request, Response } from 'express';
import type { AuthService } from '../../services/auth.js';
import { authOf, parse } from '../middleware.js';
import { emailBody, loginBody, refreshBody, registerBody, resetPasswordBody, tokenBody } from '../schemas.js';

// Identical for known and unknown emails so these endpoints can't be used to find accounts.
const CHECK_EMAIL = { message: 'If the address can receive mail, a message is on its way.' };

export function authHandlers(auth: AuthService) {
  return {
    async register(req: Request, res: Response) {
      const { email, password } = parse(registerBody, req.body);
      await auth.register(email, password);
      res.status(202).json(CHECK_EMAIL);
    },

    async verifyEmail(req: Request, res: Response) {
      await auth.verifyEmail(parse(tokenBody, req.body).token);
      res.json({ message: 'Email verified' });
    },

    async resendVerification(req: Request, res: Response) {
      await auth.resendVerification(parse(emailBody, req.body).email);
      res.status(202).json(CHECK_EMAIL);
    },

    async login(req: Request, res: Response) {
      const { email, password } = parse(loginBody, req.body);
      res.json(await auth.login(email, password));
    },

    async refresh(req: Request, res: Response) {
      res.json(await auth.refresh(parse(refreshBody, req.body).refreshToken));
    },

    async logout(req: Request, res: Response) {
      await auth.logout(parse(refreshBody, req.body).refreshToken);
      res.status(204).end();
    },

    async forgotPassword(req: Request, res: Response) {
      await auth.forgotPassword(parse(emailBody, req.body).email);
      res.status(202).json(CHECK_EMAIL);
    },

    async resetPassword(req: Request, res: Response) {
      const { token, password } = parse(resetPasswordBody, req.body);
      await auth.resetPassword(token, password);
      res.json({ message: 'Password updated' });
    },

    async me(_req: Request, res: Response) {
      res.json(await auth.me(authOf(res).accountId));
    },
  };
}
