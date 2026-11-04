#!/bin/sh
# Runs once, on a fresh mysql volume, after the image has created auth's MYSQL_DATABASE and MYSQL_USER.
# Creates the world databases and the world user, which owns only those.
set -e
mysql -uroot -p"$MYSQL_ROOT_PASSWORD" <<SQL
CREATE DATABASE IF NOT EXISTS \`$WORLD_DB_NAME\`;
CREATE DATABASE IF NOT EXISTS \`$WORLD_TEST_DB_NAME\`;
CREATE USER IF NOT EXISTS '$WORLD_DB_USER'@'%' IDENTIFIED BY '$WORLD_DB_PASSWORD';
GRANT ALL PRIVILEGES ON \`$WORLD_DB_NAME\`.* TO '$WORLD_DB_USER'@'%';
GRANT ALL PRIVILEGES ON \`$WORLD_TEST_DB_NAME\`.* TO '$WORLD_DB_USER'@'%';
SQL
