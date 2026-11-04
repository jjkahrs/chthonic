// Tests read the same .env as docker compose, then point at the dedicated test database.
try {
  process.loadEnvFile('.env');
} catch {
  // No .env: rely on the process environment.
}
