#!/bin/bash
# Generate self-signed test certificates for TLS tests.
# Output: server.crt, server.key, ca.crt, ca.key in the same directory.

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"

# Generate CA key and certificate
openssl req -x509 -newkey rsa:2048 -keyout "$DIR/ca.key" -out "$DIR/ca.crt" \
    -days 365 -nodes -subj "/CN=MiniTrantorTestCA"

# Generate server key and CSR
openssl req -newkey rsa:2048 -keyout "$DIR/server.key" -out "$DIR/server.csr" \
    -nodes -subj "/CN=localhost"

# Create extensions file for SAN
cat > "$DIR/server_ext.cnf" << EOF
[v3_req]
subjectAltName = DNS:localhost,IP:127.0.0.1
EOF

# Sign server certificate with CA
openssl x509 -req -in "$DIR/server.csr" -CA "$DIR/ca.crt" -CAkey "$DIR/ca.key" \
    -CAcreateserial -out "$DIR/server.crt" -days 365 \
    -extfile "$DIR/server_ext.cnf" -extensions v3_req

# Clean up intermediate files
rm -f "$DIR/server.csr" "$DIR/server_ext.cnf" "$DIR/ca.srl"

echo "Test certificates generated in $DIR"
