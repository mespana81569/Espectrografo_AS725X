-- Spectrograph schema. Drop & recreate is safe; user authorized full wipe.
-- Run `docker compose down -v && docker compose up -d` to apply (the
-- mysql_data volume must be empty for init.sql to re-execute).
CREATE DATABASE IF NOT EXISTS espectrografo;
USE espectrografo;

-- Identity model (R1):
--   uuid     — stable, generated on the device at MeasurementEngine::start()
--              (RFC 4122 v4 string, 36 chars).  PRIMARY KEY for joins.
--   exp_id   — user-facing label (e.g. "agua", "fabuloso2").  Indexed but
--              NOT UNIQUE — multiple experiments may share the same label.
DROP TABLE IF EXISTS absorbancias;
DROP TABLE IF EXISTS transmittances;
DROP TABLE IF EXISTS mediciones;
DROP TABLE IF EXISTS calibraciones;
DROP TABLE IF EXISTS experimentos;

CREATE TABLE experimentos (
    uuid             CHAR(36)    NOT NULL PRIMARY KEY,
    exp_id           VARCHAR(64) NOT NULL,
    timestamp_ms     BIGINT,
    num_measurements INT,
    n_cal            INT,                 -- N samples averaged for the blank ref
    gain             INT,
    mode             INT,
    int_cycles       INT,
    led_white_ma     INT,  led_ir_ma INT,  led_uv_ma INT,
    led_white_on     BOOLEAN, led_ir_on BOOLEAN, led_uv_on BOOLEAN,
    cal_valid        BOOLEAN,
    -- Snapshot of the sensor config at calibration time so the dashboard can
    -- show *why* a calibration is invalid (e.g. gain changed afterwards).
    cal_gain         INT,
    cal_int_cycles   INT,
    created_at       TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_exp_id (exp_id),
    INDEX idx_created (created_at)
);

-- Each row is one of N raw Δ-count measurements for an experiment.
CREATE TABLE mediciones (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    uuid        CHAR(36) NOT NULL,
    meas_index  INT NOT NULL,
    ch1  FLOAT, ch2  FLOAT, ch3  FLOAT, ch4  FLOAT, ch5  FLOAT, ch6  FLOAT,
    ch7  FLOAT, ch8  FLOAT, ch9  FLOAT, ch10 FLOAT, ch11 FLOAT, ch12 FLOAT,
    ch13 FLOAT, ch14 FLOAT, ch15 FLOAT, ch16 FLOAT, ch17 FLOAT, ch18 FLOAT,
    UNIQUE KEY uniq_meas (uuid, meas_index),
    FOREIGN KEY (uuid) REFERENCES experimentos(uuid) ON DELETE CASCADE
);

-- One blank-reference row per experiment (R6 in the user's clarifications:
-- the binding is always per-experiment; never shared, never inherited).
CREATE TABLE calibraciones (
    uuid CHAR(36) NOT NULL PRIMARY KEY,
    ref_ch1  FLOAT, ref_ch2  FLOAT, ref_ch3  FLOAT, ref_ch4  FLOAT,
    ref_ch5  FLOAT, ref_ch6  FLOAT, ref_ch7  FLOAT, ref_ch8  FLOAT,
    ref_ch9  FLOAT, ref_ch10 FLOAT, ref_ch11 FLOAT, ref_ch12 FLOAT,
    ref_ch13 FLOAT, ref_ch14 FLOAT, ref_ch15 FLOAT, ref_ch16 FLOAT,
    ref_ch17 FLOAT, ref_ch18 FLOAT,
    FOREIGN KEY (uuid) REFERENCES experimentos(uuid) ON DELETE CASCADE
);

-- Processed transmittance per channel per measurement, in PERCENT (0..100).
-- Computed on the ESP32 and ingested as-is by mqtt_to_db.py.
CREATE TABLE transmittances (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    uuid        CHAR(36) NOT NULL,
    meas_index  INT NOT NULL,
    ch1  FLOAT, ch2  FLOAT, ch3  FLOAT, ch4  FLOAT, ch5  FLOAT, ch6  FLOAT,
    ch7  FLOAT, ch8  FLOAT, ch9  FLOAT, ch10 FLOAT, ch11 FLOAT, ch12 FLOAT,
    ch13 FLOAT, ch14 FLOAT, ch15 FLOAT, ch16 FLOAT, ch17 FLOAT, ch18 FLOAT,
    UNIQUE KEY uniq_t (uuid, meas_index),
    FOREIGN KEY (uuid) REFERENCES experimentos(uuid) ON DELETE CASCADE
);

-- Processed absorbance per channel per measurement, dimensionless (a.u.).
-- A = -log10(T)  with NULL where T <= 0 (sensor saturation / sample fully
-- blocked).  Computed on the ESP32; ingested as-is.
CREATE TABLE absorbancias (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    uuid        CHAR(36) NOT NULL,
    meas_index  INT NOT NULL,
    ch1  FLOAT, ch2  FLOAT, ch3  FLOAT, ch4  FLOAT, ch5  FLOAT, ch6  FLOAT,
    ch7  FLOAT, ch8  FLOAT, ch9  FLOAT, ch10 FLOAT, ch11 FLOAT, ch12 FLOAT,
    ch13 FLOAT, ch14 FLOAT, ch15 FLOAT, ch16 FLOAT, ch17 FLOAT, ch18 FLOAT,
    UNIQUE KEY uniq_a (uuid, meas_index),
    FOREIGN KEY (uuid) REFERENCES experimentos(uuid) ON DELETE CASCADE
);
