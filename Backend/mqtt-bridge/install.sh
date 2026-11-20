#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
endpoint="${1:-}"
cert="${2:-$HOME/mqtt-bridge-secrets/bridge.crt}"
key="${3:-$HOME/mqtt-bridge-secrets/bridge.key}"
if [[ ! "$endpoint" =~ ^[a-z0-9-]+-ats\.iot\.ap-southeast-2\.amazonaws\.com$ ]]; then
  printf 'Usage: bash install.sh ENDPOINT [CERT_PATH] [PRIVATE_KEY_PATH]\n' >&2
  exit 1
fi
test -f "$cert" || { printf 'Certificate file not found: %s\n' "$cert" >&2; exit 1; }
test -f "$key" || { printf 'Private key file not found: %s\n' "$key" >&2; exit 1; }
id embedded-backend >/dev/null
/usr/local/bin/node --check bridge.mjs
/usr/local/bin/node --check forward.mjs
# Check the pair without printing either credential.
/usr/local/bin/node - "$cert" "$key" <<'NODE'
const fs=require('node:fs'),crypto=require('node:crypto');
const cert=new crypto.X509Certificate(fs.readFileSync(process.argv[2]));
const key=crypto.createPrivateKey(fs.readFileSync(process.argv[3]));
const options={type:'spki',format:'der'};
if(!cert.publicKey.export(options).equals(crypto.createPublicKey(key).export(options)))throw new Error('Certificate and private key do not match');
if(Date.parse(cert.validTo)<=Date.now())throw new Error('Client certificate expired');
console.log('Certificate/private-key pair: valid');
NODE
sudo install -d -o root -g embedded-backend -m 0750 /etc/embedded-backend/iot
sudo install -o embedded-backend -g embedded-backend -m 0400 "$cert" /etc/embedded-backend/iot/bridge.crt
sudo install -o embedded-backend -g embedded-backend -m 0400 "$key" /etc/embedded-backend/iot/bridge.key
curl --fail --silent --show-error --connect-timeout 10 --max-time 30 \
  https://www.amazontrust.com/repository/AmazonRootCA1.pem -o AmazonRootCA1.pem
sudo install -o root -g embedded-backend -m 0640 AmazonRootCA1.pem /etc/embedded-backend/iot/AmazonRootCA1.pem
printf '%s' "$endpoint" | sudo /usr/local/bin/node -e '
const fs=require("node:fs");
const endpoint=fs.readFileSync(0,"utf8");
fs.writeFileSync("/etc/embedded-backend/iot/bridge.json",JSON.stringify({endpoint,api_base:"https://api.embed-project.thiraphatpp.com"}),{mode:0o640});
'
sudo chown root:embedded-backend /etc/embedded-backend/iot/bridge.json
sudo install -d -m 0755 /opt/embedded-mqtt-bridge
sudo install -m 0644 bridge.mjs forward.mjs package.json package-lock.json /opt/embedded-mqtt-bridge/
# npm belongs to the existing nvm Node installation. Preserve that PATH for install.
sudo env "PATH=$PATH" npm ci --omit=dev --ignore-scripts --prefix /opt/embedded-mqtt-bridge
sudo install -m 0644 embedded-mqtt-bridge.service /etc/systemd/system/embedded-mqtt-bridge.service
sudo systemctl daemon-reload
printf '\nInstalled; NOT started. First test manually:\n'
printf 'sudo -u embedded-backend /usr/local/bin/node /opt/embedded-mqtt-bridge/bridge.mjs\n'
