/** An error that is safe to show the client. Anything else becomes a generic 500. */
export class AppError extends Error {
  constructor(
    readonly status: number,
    readonly code: string,
    message: string,
    readonly details?: unknown,
  ) {
    super(message);
  }
}

export const unauthorized = (message = 'Missing or invalid access token') => new AppError(401, 'unauthorized', message);
export const forbidden = (message = 'You do not have permission to do this') => new AppError(403, 'forbidden', message);
export const realmUnreachable = (message = 'The realm world server admin port is unreachable') =>
  new AppError(503, 'realm_unreachable', message);
