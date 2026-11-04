/** Never pass passwords, tokens or request bodies to the logger. */
export type Logger = Pick<Console, 'info' | 'warn' | 'error'>;

export const silentLogger: Logger = { info: () => {}, warn: () => {}, error: () => {} };
