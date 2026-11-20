'use strict';
const createStations = require('./stations.cjs');
const {dateRange} = require('./time.cjs');
const fields = 'sample_id, device_id, measured_at, received_at, raw_payload';
const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);

function json(res, status, body) {
  res.writeHead(status, {'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store'});
  res.end(JSON.stringify(body));
}
function body(req) {
  return new Promise((resolve, reject) => {
    let chunks = [], size = 0, failed = false;
    const fail = (status, message) => {
      if (failed) return;
      failed = true; chunks = []; clearTimeout(timer);
      reject(Object.assign(new Error(message), {status}));
    };
    const timer = setTimeout(() => fail(408, 'request_timeout'), 10000);
    req.on('data', chunk => {
      size += chunk.length;
      if (size > 16384) fail(413, 'payload_too_large');
      if (!failed) chunks.push(chunk);
    });
    req.on('end', () => {
      clearTimeout(timer);
      if (failed) return;
      try {resolve(JSON.parse(Buffer.concat(chunks).toString('utf8')));}
      catch {reject(Object.assign(new Error('invalid_json'), {status: 400}));}
    });
    req.on('error', () => fail(400, 'request_error'));
    req.on('aborted', () => fail(400, 'request_aborted'));
  });
}

module.exports = function createHandler(pool, options = {}) {
  const stations = createStations(pool, json);
  const origins = new Set(options.origins || []);
  return async (req, res) => {
    res.setHeader('X-Content-Type-Options', 'nosniff');
    try {
      const origin = req.headers.origin;
      if (origin && !origins.has(origin)) {
        req.resume(); json(res, 403, {error: 'dashboard_origin_not_allowed'}); return;
      }
      if (origin) {
        res.setHeader('Access-Control-Allow-Origin', origin);
        res.setHeader('Vary', 'Origin');
        res.setHeader('Access-Control-Allow-Methods', 'GET, POST, PATCH, OPTIONS');
        res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
      }
      if (req.method === 'OPTIONS') {req.resume();res.writeHead(204);res.end();return;}
      const url = new URL(req.url, 'http://localhost');
      const route = url.pathname;
      if (req.method === 'GET' && route === '/') {
        json(res, 200, {service: 'embedded-api', health: '/health', api_version: 1});return;
      }
      if (req.method === 'GET' && route === '/health') {
        const result = await pool.query('SELECT current_database() AS database_name');
        json(res, 200, {status: 'ok', database: 'connected', database_name: result.rows[0].database_name}); return;
      }
      if (await stations(req, res, url, body)) return;
      if (req.method === 'GET' && route === '/api/devices') {
        const stationId=url.searchParams.get('station_id');
        if(stationId!==null&&(!/^\d{1,10}$/.test(stationId)||+stationId<1||+stationId>2147483647)){
          json(res,400,{error:'invalid_station_id'});return;
        }
        const result = await pool.query(`SELECT d.device_id, d.dispenser_id, d.role, d.is_simulated,
          t.sample_id AS latest_sample_id, t.received_at AS latest_received_at
          FROM device d LEFT JOIN LATERAL (
            SELECT sample_id, received_at FROM telemetry_sample WHERE device_id = d.device_id
            ORDER BY received_at DESC, sample_id DESC LIMIT 1
          ) t ON true WHERE ($1::integer IS NULL OR d.dispenser_id=$1)
          ORDER BY d.device_id LIMIT 200`,[stationId]);
        json(res, 200, {devices: result.rows}); return;
      }
      if (req.method === 'GET' && route === '/api/telemetry') {
        const limitText = url.searchParams.get('limit') ?? '20';
        if (!/^\d{1,3}$/.test(limitText) || +limitText < 1 || +limitText > 100) {
          json(res, 400, {error: 'limit_must_be_1_to_100'}); return;
        }
        const device = url.searchParams.get('device_id');
        const range = dateRange(url);
        const result = await pool.query(`SELECT ${fields} FROM telemetry_sample
          WHERE ($1::text IS NULL OR device_id = $1)
          AND ($3::timestamptz IS NULL OR received_at >= $3)
          AND ($4::timestamptz IS NULL OR received_at < $4)
          ORDER BY received_at DESC, sample_id DESC LIMIT $2`, [device, +limitText, range.from, range.to_exclusive]);
        json(res, 200, {samples: result.rows, range}); return;
      }
      if (req.method === 'GET' && /^\/api\/telemetry\/[0-9]+$/.test(route)) {
        const id = route.split('/').pop();
        if (id.length > 19 || BigInt(id) > 9223372036854775807n) {
          json(res, 400, {error: 'invalid_sample_id'}); return;
        }
        const result = await pool.query(`SELECT ${fields} FROM telemetry_sample WHERE sample_id = $1`, [id]);
        json(res, result.rows.length ? 200 : 404, result.rows[0] || {error: 'sample_not_found'}); return;
      }
      if (req.method === 'POST' && route === '/api/telemetry') {
        if ((req.headers['content-type'] || '').split(';')[0].trim().toLowerCase() !== 'application/json') {
          req.resume(); json(res, 415, {error: 'use_application_json'}); return;
        }
        const message = await body(req);
        const allowed = new Set(['schema_version', 'device_id', 'measured_at', 'raw_payload', 'payload']);
        const payload = message?.raw_payload ?? message?.payload;
        if (!object(message) || Object.keys(message).some(key => !allowed.has(key)) ||
            message.schema_version !== 1 || typeof message.device_id !== 'string' ||
            message.device_id.length < 1 || message.device_id.length > 128 ||
            !object(payload) || ('raw_payload' in message && 'payload' in message)) {
          json(res, 400, {error: 'invalid_message', expected: {schema_version: 1, device_id: 'registered-device-id', measured_at: null, raw_payload: {test_data: true, demo_value: 42}}}); return;
        }
        const measured = message.measured_at ?? null;
        if (measured !== null && (typeof measured !== 'string' ||
            !/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d{1,6})?(Z|[+-]\d{2}:\d{2})$/.test(measured) ||
            !Number.isFinite(Date.parse(measured)))) {
          json(res, 400, {error: 'measured_at_must_be_ISO8601_with_timezone_or_null'}); return;
        }
        const result = await pool.query(`INSERT INTO telemetry_sample (device_id, measured_at, raw_payload)
          VALUES ($1, $2, $3::jsonb) RETURNING ${fields}`,
          [message.device_id, measured, JSON.stringify(payload)]);
        res.setHeader('Location', `/api/telemetry/${result.rows[0].sample_id}`);
        json(res, 201, {stored: true, sample: result.rows[0]}); return;
      }
      req.resume();
      if (route === '/health' || route === '/api/devices' || route === '/api/telemetry') {
        res.setHeader('Allow', route === '/api/telemetry' ? 'GET, POST' : 'GET');
        json(res, 405, {error: 'method_not_allowed'}); return;
      }
      json(res, 404, {error: 'not_found'});
    } catch (error) {
      if (error.code === '23503') {json(res, 422, {error: 'device_not_registered'}); return;}
      if (error.code === '22007' || error.code === '22008') {json(res, 400, {error: 'invalid_timestamp'}); return;}
      if (error.status) {res.setHeader('Connection', 'close'); json(res, error.status, {error: error.message}); return;}
      console.error('Request failed:', error.code || error.name);
      json(res, 503, {error: 'database_unavailable'});
    }
  };
};
