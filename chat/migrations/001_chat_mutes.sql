-- GM mutes (docs/chat/security.html#mutes). One row per muted character name, any world of the realm.
-- name_key: the name in ASCII lower case (names are unique case-insensitively); until_ms: Unix ms, wall clock (UTC).
CREATE TABLE IF NOT EXISTS chat_mutes (
  name_key   VARCHAR(16)     NOT NULL PRIMARY KEY,
  name       VARCHAR(16)     NOT NULL,
  until_ms   BIGINT UNSIGNED NOT NULL,
  muted_by   BIGINT UNSIGNED NOT NULL,
  created_at DATETIME(3)     NOT NULL,
  KEY idx_chat_mutes_until (until_ms)
);
