-- Internal admin endpoint of each realm's world server, used by the admin service.
-- Never returned by the public GET /v1/realms.
ALTER TABLE realms
  ADD COLUMN admin_host VARCHAR(255)      NULL AFTER port,
  ADD COLUMN admin_port SMALLINT UNSIGNED NULL AFTER admin_host;
