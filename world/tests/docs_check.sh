#!/usr/bin/env bash
# Cross-checks the docs against the source: every opcode and code in protocol/messages.h must be in the protocol page
# with the same value, and every env var read by server/core/config.cpp (and .env.example) must be in the world docs.
# The chat service's (../chat): its opcodes and notice codes in docs/chat/protocol.html, its env vars
# (server/config.cpp, .env.example) in docs/chat/*.html.
set -u
cd "$(dirname "$0")/.."
fail=0
DOCS=../docs/world
CHAT_DOCS=../docs/chat
CHAT=../chat
# Table cells are one per line in the HTML; join them so a row matches on one line (CRLF checkouts too).
protocol=$(tr -d '\r' < "$DOCS/protocol.html" | tr '\n' ' ')
docs=$(cat "$DOCS"/*.html)
chat_protocol=$(tr -d '\r' < "$CHAT_DOCS/protocol.html" | tr '\n' ' ')
chat_docs=$(cat "$CHAT_DOCS"/*.html)

echo "== opcodes: protocol/messages.h -> $DOCS/protocol.html"
while read -r name _ hex; do
    if grep -q "<td><code>$hex</code></td> <td>$name</td>" <<< "$protocol"; then echo "  ok      $hex $name"; else echo "  MISSING $hex $name"; fail=1; fi
done < <(grep -oE '^\s+[A-Za-z]+ = 0x[0-9A-F]{4}' protocol/messages.h)

echo "== codes: protocol/messages.h -> $DOCS/protocol.html"
while read -r name _ val; do
    if grep -q "<td>$val</td> <td><code>$name</code></td>" <<< "$protocol"; then echo "  ok      $val $name"; else echo "  MISSING $val $name"; fail=1; fi
done < <(sed -n '/^enum class Code /,/^};/p' protocol/messages.h | grep -oE '^\s+[a-z_]+ = [0-9]+,' | sed -n '/protocol_mismatch/,$p' | tr -d ',')

echo "== chat opcodes: protocol/messages.h -> $CHAT_DOCS/protocol.html"
while read -r name _ hex; do
    if grep -q "<td><code>$hex</code></td> <td>$name</td>" <<< "$chat_protocol"; then echo "  ok      $hex $name"; else echo "  MISSING $hex $name"; fail=1; fi
done < <(grep -oE '^\s+(Chat[A-Za-z]*|Link[A-Za-z]*|CharOnline|MapChanged|CharOffline|MuteState) = 0x[0-9A-F]{4}' protocol/messages.h)

echo "== chat notice codes: protocol/messages.h -> $CHAT_DOCS/protocol.html"
while read -r name _ val; do
    if grep -q "<td>$val</td> <td><code>$name</code></td>" <<< "$chat_protocol"; then echo "  ok      $val $name"; else echo "  MISSING $val $name"; fail=1; fi
done < <(sed -n '/^enum class ChatNoticeCode /,/^};/p' protocol/messages.h | grep -oE '^\s+[a-z_]+ = [0-9]+,' | tr -d ',')

echo "== env vars: server/core/config.cpp + .env.example -> $DOCS/*.html"
n=0
while read -r v; do
    n=$((n + 1))
    if grep -q "<code>$v</code>" <<< "$docs"; then echo "  ok      $v"; else echo "  MISSING $v"; fail=1; fi
done < <({ grep -oE '"[A-Z][A-Z0-9_]+"' server/core/config.cpp | tr -d '"'; grep -oE '^[A-Z_]+' .env.example; } | sort -u)
echo "  ($n variables)"

echo "== chat env vars: $CHAT/server/config.cpp + $CHAT/.env.example -> $CHAT_DOCS/*.html"
n=0
while read -r v; do
    n=$((n + 1))
    if grep -q "<code>$v</code>" <<< "$chat_docs"; then echo "  ok      $v"; else echo "  MISSING $v"; fail=1; fi
done < <({ grep -oE '"[A-Z][A-Z0-9_]+"' "$CHAT/server/config.cpp" | tr -d '"'; grep -oE '^[A-Z_]+' "$CHAT/.env.example"; } | sort -u)
echo "  ($n variables)"

[ $fail -eq 0 ] && echo "PASS docs check" || echo "FAIL docs check"
exit $fail
