-- Characters per realm. Names are ASCII letters; ascii_general_ci makes the unique key case-insensitive.
-- The unique key covers soft-deleted rows too, so a deleted character's name stays reserved.
CREATE TABLE characters (
  id             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  realm_id       INT UNSIGNED    NOT NULL,
  account_id     BIGINT UNSIGNED NOT NULL,
  name           VARCHAR(16) CHARACTER SET ascii COLLATE ascii_general_ci NOT NULL,
  pos_x          DOUBLE          NOT NULL,
  pos_y          DOUBLE          NOT NULL,
  pos_z          DOUBLE          NOT NULL,
  heading        DOUBLE          NOT NULL,
  created_at     DATETIME(3)     NOT NULL,
  last_login_at  DATETIME(3)     NULL,
  deleted_at     DATETIME(3)     NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uq_characters_realm_name (realm_id, name),
  KEY ix_characters_account (realm_id, account_id, deleted_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
