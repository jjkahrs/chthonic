// Tests read admin/.env, then point at the dedicated test databases (see test/helpers.ts).
try {
  process.loadEnvFile('.env');
} catch {
  // No .env: rely on the process environment.
}
