-- One row per mutating admin API request, success or failure. result is 'ok' or the error code.
CREATE TABLE admin_audit (
  id                BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  actor_account_id  BIGINT UNSIGNED NOT NULL,
  actor_email       VARCHAR(254)    NOT NULL,
  actor_role        VARCHAR(16)     NOT NULL,
  action            VARCHAR(64)     NOT NULL,
  target_type       VARCHAR(32)     NOT NULL,
  target_id         VARCHAR(64)     NULL,
  `before`          JSON            NULL,
  `after`           JSON            NULL,
  forced            BOOLEAN         NOT NULL DEFAULT FALSE,
  result            VARCHAR(64)     NOT NULL,
  created_at        DATETIME(3)     NOT NULL,
  PRIMARY KEY (id),
  KEY ix_admin_audit_created (created_at),
  KEY ix_admin_audit_actor (actor_account_id, created_at),
  KEY ix_admin_audit_target (target_type, target_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Each realm's world admin endpoint, copied from auth whenever an admin reads realms. Auth's realm list is admin-only,
-- so GM requests (kick, who is online, the character write guard) look the endpoint up here.
CREATE TABLE realm_endpoints (
  realm_id    INT UNSIGNED      NOT NULL,
  name        VARCHAR(64)       NOT NULL,
  admin_host  VARCHAR(255)      NULL,
  admin_port  SMALLINT UNSIGNED NULL,
  synced_at   DATETIME(3)       NOT NULL,
  PRIMARY KEY (realm_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
