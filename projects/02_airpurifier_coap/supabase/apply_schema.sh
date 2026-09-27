#!/bin/sh
# Apply schema.sql (purifier_* objects only) to the shared Supabase project. Idempotent.
# Needs DATABASE_URL (session pooler string) in projects/02_airpurifier_coap/.env.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
[ -f "$DIR/../.env" ] && { set -a; . "$DIR/../.env"; set +a; }
[ -n "$DATABASE_URL" ] || { echo "DATABASE_URL is not set - see ../.env.template" >&2; exit 1; }
PSQL=$(command -v psql || echo /Applications/Postgres.app/Contents/Versions/18/bin/psql)
[ -x "$PSQL" ] || { echo "psql not found" >&2; exit 1; }
echo "applying $DIR/schema.sql ..."
"$PSQL" "$DATABASE_URL" -v ON_ERROR_STOP=1 -q -f "$DIR/schema.sql"
echo "schema applied"
