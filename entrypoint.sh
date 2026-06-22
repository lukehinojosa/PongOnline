#!/bin/bash
set -e

if [ -z "$DOMAIN" ]; then
    echo "[entrypoint] No DOMAIN set — running plain ws:// on port 9000"
    exec /app/pong_signaling
fi

CERT_DIR="/etc/letsencrypt/live/$DOMAIN"

if [ ! -f "$CERT_DIR/fullchain.pem" ]; then
    echo "[entrypoint] Obtaining TLS cert for $DOMAIN..."
    certbot certonly \
        --standalone \
        --non-interactive \
        --agree-tos \
        --email "${CERT_EMAIL:-admin@example.com}" \
        -d "$DOMAIN"
else
    echo "[entrypoint] Cert found — attempting renewal if needed..."
    certbot renew --quiet --standalone
fi

# Daily renewal in the background
echo "0 3 * * * certbot renew --quiet --standalone && kill -HUP 1 2>/dev/null || true" | crontab -
cron

export TLS_CERT="$CERT_DIR/fullchain.pem"
export TLS_KEY="$CERT_DIR/privkey.pem"

exec /app/pong_signaling
