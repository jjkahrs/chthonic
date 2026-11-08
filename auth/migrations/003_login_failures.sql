-- Failed-login counter and lockout per (account, client IP), so a stranger's failures
-- never lock the owner out from their own address. Replaces accounts.failed_login_count /
-- accounts.locked_until, which are left in place but no longer read or written.
CREATE TABLE login_failures (
  account_id   BIGINT UNSIGNED NOT NULL,
  ip           VARCHAR(64)     NOT NULL,
  failed_count INT UNSIGNED    NOT NULL DEFAULT 0,
  locked_until DATETIME(3)     NULL,
  updated_at   DATETIME(3)     NOT NULL,
  PRIMARY KEY (account_id, ip),
  KEY ix_login_failures_updated (updated_at),
  CONSTRAINT fk_login_failures_account FOREIGN KEY (account_id) REFERENCES accounts (id) ON DELETE CASCADE
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;
