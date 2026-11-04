#!/bin/sh
# Run by the compose service chat-db-init before every chat start (fresh or existing mysql volume): the chat service's
# database and user, which owns only that database. Idempotent.
set -e
mysql -h mysql -uroot -p"$MYSQL_ROOT_PASSWORD" <<SQL
CREATE DATABASE IF NOT EXISTS \`$CHAT_DB_NAME\`;
CREATE USER IF NOT EXISTS '$CHAT_DB_USER'@'%' IDENTIFIED BY '$CHAT_DB_PASSWORD';
GRANT ALL PRIVILEGES ON \`$CHAT_DB_NAME\`.* TO '$CHAT_DB_USER'@'%';
SQL
echo "chat database $CHAT_DB_NAME ready"
