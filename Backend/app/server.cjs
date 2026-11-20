const http = require('node:http');
const fs = require('node:fs');
const {Pool} = require('pg');
const createHandler = require('./api.cjs');
const config = JSON.parse(fs.readFileSync('/etc/embedded-backend/database.json', 'utf8'));
if (config.database !== 'embedded') throw new Error('Expected database embedded; run preflight first.');
const pool = new Pool({...config,
  ssl: {ca: fs.readFileSync('/etc/embedded-backend/rds-ca.pem', 'utf8'), rejectUnauthorized: true},
  max: 5, connectionTimeoutMillis: 5000, idleTimeoutMillis: 30000,
  statement_timeout: 5000, query_timeout: 6000
});
pool.on('error', error => console.error('Pool error:', error.code));
const originFile='/etc/embedded-backend/dashboard-origins.json';
const origins = fs.existsSync(originFile)?JSON.parse(fs.readFileSync(originFile,'utf8')):[];
if (!Array.isArray(origins) || origins.some(origin => typeof origin !== 'string' || !/^https?:\/\//.test(origin) || new URL(origin).origin !== origin)) {
  throw new Error('dashboard-origins.json must contain exact http/https origins without paths');
}
const server = http.createServer(createHandler(pool, {origins}));
server.requestTimeout = 15000;
server.headersTimeout = 10000;
server.on('error', error => {console.error('Server error:', error.code); process.exit(1);});
server.listen(3000, '127.0.0.1', () => console.log('Telemetry backend listening on 127.0.0.1:3000; database=embedded'));
function shutdown() {
  server.close(async () => {await pool.end(); process.exit(0);});
  setTimeout(() => process.exit(1), 10000).unref();
}
process.once('SIGTERM', shutdown);
process.once('SIGINT', shutdown);
