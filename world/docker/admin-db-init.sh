#!/bin/sh
# Run by the compose service admin-db-init before every admin start (fresh or existing mysql volume), after auth and
# world have migrated. Idempotent. Creates:
#  - the admin service's audit database and its user, which owns only that database;
#  - the admin service's world user: SELECT and UPDATE on the characters table only;
#  - this stack's realm in auth's realms table, with its world admin port (kept if an admin already set one).
set -e
mysql -h mysql -uroot -p"$MYSQL_ROOT_PASSWORD" <<SQL
CREATE DATABASE IF NOT EXISTS \`$ADMIN_DB_NAME\`;
CREATE USER IF NOT EXISTS '$ADMIN_DB_USER'@'%' IDENTIFIED BY '$ADMIN_DB_PASSWORD';
GRANT ALL PRIVILEGES ON \`$ADMIN_DB_NAME\`.* TO '$ADMIN_DB_USER'@'%';
CREATE USER IF NOT EXISTS '$ADMIN_WORLD_DB_USER'@'%' IDENTIFIED BY '$ADMIN_WORLD_DB_PASSWORD';
GRANT SELECT, UPDATE ON \`$WORLD_DB_NAME\`.characters TO '$ADMIN_WORLD_DB_USER'@'%';
INSERT INTO \`$AUTH_DB_NAME\`.realms (id, name, host, port, admin_host, admin_port, region, status, created_at, updated_at)
VALUES ($REALM_ID, 'Local', '127.0.0.1', $WORLD_HOST_PORT, 'world', 7780, 'local', 'online', UTC_TIMESTAMP(3), UTC_TIMESTAMP(3))
ON DUPLICATE KEY UPDATE
  admin_host = COALESCE(admin_host, VALUES(admin_host)),
  admin_port = COALESCE(admin_port, VALUES(admin_port));
SQL
echo "admin databases ready; realm $REALM_ID admin port seeded"
