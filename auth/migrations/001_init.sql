-- All timestamps are UTC, written by the application (never NOW()) so an injected clock controls them.

CREATE TABLE accounts (
  id                 BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  email              VARCHAR(254)    NOT NULL,
  password_hash      CHAR(60)        NOT NULL,
  role               ENUM('player', 'gm', 'admin') NOT NULL DEFAULT 'player',
  email_verified_at  DATETIME(3)     NULL,
  failed_login_count INT UNSIGNED    NOT NULL DEFAULT 0,
  locked_until       DATETIME(3)     NULL,
  created_at         DATETIME(3)     NOT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uq_accounts_email (email)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_0900_ai_ci;

CREATE TABLE refresh_tokens (
  id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  account_id BIGINT UNSIGNED NOT NULL,
  family_id  CHAR(32)        NOT NULL,
  token_hash CHAR(64)        NOT NULL,
  created_at DATETIME(3)     NOT NULL,
  expires_at DATETIME(3)     NOT NULL,
  rotated_at DATETIME(3)     NULL,
  revoked_at DATETIME(3)     NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uq_refresh_tokens_hash (token_hash),
  KEY ix_refresh_tokens_family (family_id),
  KEY ix_refresh_tokens_account (account_id),
  CONSTRAINT fk_refresh_tokens_account FOREIGN KEY (account_id) REFERENCES accounts (id) ON DELETE CASCADE
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

CREATE TABLE email_tokens (
  id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  account_id BIGINT UNSIGNED NOT NULL,
  purpose    ENUM('verify_email', 'password_reset') NOT NULL,
  token_hash CHAR(64)        NOT NULL,
  created_at DATETIME(3)     NOT NULL,
  expires_at DATETIME(3)     NOT NULL,
  used_at    DATETIME(3)     NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uq_email_tokens_hash (token_hash),
  KEY ix_email_tokens_account (account_id, purpose),
  CONSTRAINT fk_email_tokens_account FOREIGN KEY (account_id) REFERENCES accounts (id) ON DELETE CASCADE
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

CREATE TABLE bans (
  id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  account_id BIGINT UNSIGNED NOT NULL,
  reason     VARCHAR(500)    NOT NULL,
  expires_at DATETIME(3)     NULL,
  banned_by  BIGINT UNSIGNED NULL,
  created_at DATETIME(3)     NOT NULL,
  lifted_at  DATETIME(3)     NULL,
  lifted_by  BIGINT UNSIGNED NULL,
  PRIMARY KEY (id),
  KEY ix_bans_account (account_id, lifted_at),
  CONSTRAINT fk_bans_account FOREIGN KEY (account_id) REFERENCES accounts (id) ON DELETE CASCADE,
  CONSTRAINT fk_bans_banned_by FOREIGN KEY (banned_by) REFERENCES accounts (id) ON DELETE SET NULL,
  CONSTRAINT fk_bans_lifted_by FOREIGN KEY (lifted_by) REFERENCES accounts (id) ON DELETE SET NULL
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;

CREATE TABLE realms (
  id         INT UNSIGNED      NOT NULL AUTO_INCREMENT,
  name       VARCHAR(64)       NOT NULL,
  host       VARCHAR(255)      NOT NULL,
  port       SMALLINT UNSIGNED NOT NULL,
  region     VARCHAR(32)       NOT NULL,
  status     ENUM('online', 'offline', 'maintenance') NOT NULL DEFAULT 'offline',
  sort_order INT               NOT NULL DEFAULT 0,
  created_at DATETIME(3)       NOT NULL,
  updated_at DATETIME(3)       NOT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uq_realms_name (name),
  KEY ix_realms_sort (sort_order, id)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_0900_ai_ci;

-- Fixed-window per-IP counters. Rows past expires_at are pruned on write.
CREATE TABLE rate_limits (
  bucket       VARCHAR(32)  NOT NULL,
  ip           VARCHAR(64)  NOT NULL,
  window_start DATETIME(3)  NOT NULL,
  hits         INT UNSIGNED NOT NULL,
  expires_at   DATETIME(3)  NOT NULL,
  PRIMARY KEY (bucket, ip, window_start),
  KEY ix_rate_limits_expires (expires_at)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4;
