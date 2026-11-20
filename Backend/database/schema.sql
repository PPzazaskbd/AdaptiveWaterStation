-- Run while connected to the embedded database. Never drops existing data.
-- Existing tables must match this project's schema; IF NOT EXISTS is not a migration.
BEGIN;

CREATE TABLE IF NOT EXISTS dispenser (
    dispenser_id integer GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    name text NOT NULL,
    location text,
    tank_capacity_ml numeric(12, 2) NOT NULL CHECK (tank_capacity_ml > 0)
);

CREATE TABLE IF NOT EXISTS device (
    device_id text PRIMARY KEY,
    dispenser_id integer NOT NULL REFERENCES dispenser(dispenser_id),
    role text NOT NULL CHECK (role IN ('NODE', 'GATEWAY')),
    is_simulated boolean NOT NULL DEFAULT false,
    last_seen_at timestamptz
);
CREATE INDEX IF NOT EXISTS device_dispenser_id_idx ON device (dispenser_id);

CREATE TABLE IF NOT EXISTS telemetry_sample (
    sample_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    device_id text NOT NULL REFERENCES device(device_id),
    measured_at timestamptz,
    received_at timestamptz NOT NULL DEFAULT CURRENT_TIMESTAMP,
    raw_payload jsonb NOT NULL CHECK (jsonb_typeof(raw_payload) = 'object')
);
CREATE INDEX IF NOT EXISTS telemetry_sample_device_time_idx
    ON telemetry_sample (device_id, measured_at DESC);
CREATE INDEX IF NOT EXISTS telemetry_sample_received_at_idx
    ON telemetry_sample (received_at DESC);

CREATE TABLE IF NOT EXISTS fill_session (
    fill_id integer GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    dispenser_id integer NOT NULL REFERENCES dispenser(dispenser_id),
    started_at timestamptz NOT NULL,
    ended_at timestamptz,
    volume_ml numeric(12, 2) CHECK (volume_ml >= 0),
    CHECK (ended_at IS NULL OR ended_at >= started_at)
);
CREATE INDEX IF NOT EXISTS fill_session_dispenser_time_idx
    ON fill_session (dispenser_id, started_at DESC);

CREATE TABLE IF NOT EXISTS tank_refill (
    refill_id integer GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    dispenser_id integer NOT NULL REFERENCES dispenser(dispenser_id),
    refilled_at timestamptz NOT NULL,
    added_volume_ml numeric(12, 2) NOT NULL CHECK (added_volume_ml > 0)
);
CREATE INDEX IF NOT EXISTS tank_refill_dispenser_time_idx
    ON tank_refill (dispenser_id, refilled_at DESC);

CREATE TABLE IF NOT EXISTS system_status (
    dispenser_id integer PRIMARY KEY REFERENCES dispenser(dispenser_id),
    last_seen_at timestamptz,
    operation_status text CHECK (operation_status IN ('STANDBY', 'DISPENSING')),
    water_updated_at timestamptz,
    remaining_water_ml numeric(12, 2) CHECK (remaining_water_ml >= 0),
    CHECK ((remaining_water_ml IS NULL) = (water_updated_at IS NULL))
);

-- Create exactly one initial status row per dispenser. Unknown values stay NULL.
CREATE OR REPLACE FUNCTION initialize_dispenser_status()
RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO system_status (dispenser_id) VALUES (NEW.dispenser_id)
    ON CONFLICT (dispenser_id) DO NOTHING;
    RETURN NEW;
END;
$$;

CREATE OR REPLACE TRIGGER dispenser_initialize_status
AFTER INSERT ON dispenser
FOR EACH ROW EXECUTE FUNCTION initialize_dispenser_status();

INSERT INTO system_status (dispenser_id)
SELECT dispenser_id FROM dispenser
ON CONFLICT (dispenser_id) DO NOTHING;

COMMIT;
