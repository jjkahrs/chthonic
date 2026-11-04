#!/bin/sh
# Runs once, on a fresh mysql volume, after the image has created MYSQL_DATABASE and MYSQL_USER.
# Creates the separate database used by `npm test` and lets the app user own it.
set -e
mysql -uroot -p"$MYSQL_ROOT_PASSWORD" <<SQL
CREATE DATABASE IF NOT EXISTS \`$TEST_DB_NAME\`;
GRANT ALL PRIVILEGES ON \`$TEST_DB_NAME\`.* TO '$MYSQL_USER'@'%';
SQL
