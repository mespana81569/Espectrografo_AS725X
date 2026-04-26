"""MQTT -> MySQL bridge for the spectrograph.

Identity model (R1): every experiment has a UUID generated on the ESP32 at
acquisition start.  The bridge keys all inserts on `uuid` and treats `exp_id`
as a user-facing label only.  This eliminates the silent INSERT IGNORE
de-duplication that previously dropped 12 of 15 experiments named "agua",
"agua2", ... when the device's exp_id wasn't refreshed between saves.

The ESP32 computes transmittance (%) and absorbance (a.u.) before publishing
(see "ESP32 feasibility verdict" in the design doc).  The bridge stores the
processed values exactly as received — no recomputation, no daemon.
"""
import json
import math
import os
import paho.mqtt.client as mqtt
import mysql.connector

MQTT_BROKER = os.getenv("MQTT_BROKER", "mosquitto")
MQTT_PORT   = int(os.getenv("MQTT_PORT", "1883"))   # docker-compose default
DB_CONFIG = {
    "host":     os.getenv("MYSQL_HOST",     "mysql"),
    "user":     os.getenv("MYSQL_USER",     "root"),
    "password": os.getenv("MYSQL_PASSWORD", ""),
    "database": os.getenv("MYSQL_DATABASE", "espectrografo"),
}

NCH = 18

def get_db():
    return mysql.connector.connect(**DB_CONFIG)

def _row_18(values, ch_offset=0):
    """Pad/truncate a list to exactly 18 floats; missing values become None."""
    out = list(values)[:NCH] if values else []
    while len(out) < NCH:
        out.append(None)
    return out

def upsert_experiment(cursor, data):
    sensor = data.get("sensor", {})
    cal    = data.get("calibration", {})
    cal_cfg = cal.get("cfg_at_cal", {})
    cursor.execute("""
        INSERT INTO experimentos
          (uuid, exp_id, timestamp_ms, num_measurements, n_cal,
           gain, mode, int_cycles,
           led_white_ma, led_ir_ma, led_uv_ma,
           led_white_on, led_ir_on, led_uv_on,
           cal_valid, cal_gain, cal_int_cycles)
        VALUES (%s,%s,%s,%s,%s, %s,%s,%s, %s,%s,%s, %s,%s,%s, %s,%s,%s)
        ON DUPLICATE KEY UPDATE
           exp_id           = VALUES(exp_id),
           timestamp_ms     = VALUES(timestamp_ms),
           num_measurements = VALUES(num_measurements),
           n_cal            = VALUES(n_cal),
           gain             = VALUES(gain),
           mode             = VALUES(mode),
           int_cycles       = VALUES(int_cycles),
           led_white_ma     = VALUES(led_white_ma),
           led_ir_ma        = VALUES(led_ir_ma),
           led_uv_ma        = VALUES(led_uv_ma),
           led_white_on     = VALUES(led_white_on),
           led_ir_on        = VALUES(led_ir_on),
           led_uv_on        = VALUES(led_uv_on),
           cal_valid        = VALUES(cal_valid),
           cal_gain         = VALUES(cal_gain),
           cal_int_cycles   = VALUES(cal_int_cycles)
    """, (
        data["uuid"],
        data.get("exp_id", ""),
        data.get("timestamp_ms", 0),
        data.get("num_measurements", 0),
        data.get("n_cal", 0),
        sensor.get("gain", 0),
        sensor.get("mode", 0),
        sensor.get("int_cycles", 0),
        sensor.get("led_white_ma", 0),
        sensor.get("led_ir_ma", 0),
        sensor.get("led_uv_ma", 0),
        bool(sensor.get("led_white_on", False)),
        bool(sensor.get("led_ir_on", False)),
        bool(sensor.get("led_uv_on", False)),
        bool(cal.get("valid", False)),
        cal_cfg.get("gain", 0),
        cal_cfg.get("int_cycles", 0),
    ))

def upsert_calibration(cursor, uuid, offsets):
    refs = _row_18(offsets)
    cursor.execute(
        "REPLACE INTO calibraciones (uuid, " +
        ",".join(f"ref_ch{i}" for i in range(1, NCH + 1)) +
        ") VALUES (%s," + ",".join(["%s"] * NCH) + ")",
        (uuid, *refs)
    )

def insert_rows(cursor, table, uuid, rows):
    """Insert one row per measurement.  REPLACE so a re-publish of the same
    (uuid, meas_index) updates rather than duplicates — keeps the bridge
    idempotent under MQTT retry storms."""
    if not rows:
        return
    cols = ",".join(f"ch{i}" for i in range(1, NCH + 1))
    placeholders = ",".join(["%s"] * NCH)
    sql = f"REPLACE INTO {table} (uuid, meas_index, {cols}) VALUES (%s,%s,{placeholders})"
    for i, row in enumerate(rows):
        cursor.execute(sql, (uuid, i, *_row_18(row)))

def _compute_ta(spectra, offsets):
    """Recompute (transmittance %, absorbance a.u.) from raw Δ + I0.

    Bulk-upload payloads from the ESP32 ship raw + offsets only — T+A are
    intentionally omitted to keep MQTT packets under the buffer limit (a 20-
    measurement experiment with raw+T+A blew past PubSubClient's 4 KB cap
    and silently dropped via publish() -> false).  Bridge recomputes the
    same math the device runs in MeasurementEngine::computeProcessed():

        I  = Δ + I0           (counts; clamped at 0 — no negative photons)
        T  = I / I0 * 100     (percent)
        A  = -log10(T / 100)  (a.u.; NULL if T <= 0)

    Live publishes (esp32/data/spectra) DO carry T+A — those rows are used
    as-is when present, otherwise we fall through to the recompute path.
    """
    if not offsets or len(offsets) < NCH:
        return [], []
    T_rows, A_rows = [], []
    for row in spectra:
        if not row: continue
        T_row, A_row = [], []
        for c in range(NCH):
            I0 = float(offsets[c] or 0)
            d  = float(row[c]    or 0) if c < len(row) else 0.0
            if I0 <= 0:
                T_row.append(None); A_row.append(None); continue
            I = d + I0
            if I < 0: I = 0
            T = I / I0 * 100.0
            T_row.append(T)
            A_row.append(-math.log10(T / 100.0) if T > 0 else None)
        T_rows.append(T_row); A_rows.append(A_row)
    return T_rows, A_rows

def store_experiment(data):
    if "uuid" not in data:
        print(f"[bridge] reject: payload missing uuid (exp_id={data.get('exp_id')!r})")
        return
    spectra = data.get("spectra", []) or []
    offsets = (data.get("calibration") or {}).get("offsets") or []
    # Prefer device-supplied T+A (live spectra publishes) — they were
    # computed from the same raw values, but on the firmware's clock and
    # may carry NaN-as-null for channels we couldn't.  If absent (bulk
    # upload payloads), recompute from raw + offsets so the DB is always
    # populated with processed values regardless of which path delivered
    # the experiment.
    trans  = data.get("transmittance") or []
    absorb = data.get("absorbance")    or []
    if not trans or not absorb:
        trans2, absorb2 = _compute_ta(spectra, offsets)
        if not trans:  trans  = trans2
        if not absorb: absorb = absorb2

    db = get_db()
    cursor = db.cursor()
    try:
        upsert_experiment(cursor, data)
        upsert_calibration(cursor, data["uuid"], offsets)
        insert_rows(cursor, "mediciones",     data["uuid"], spectra)
        insert_rows(cursor, "transmittances", data["uuid"], trans)
        insert_rows(cursor, "absorbancias",   data["uuid"], absorb)
        db.commit()
        print(f"[bridge] stored uuid={data['uuid']} exp_id={data.get('exp_id')} "
              f"N={len(spectra)} T={'live' if data.get('transmittance') else 'recomputed'}")
    except Exception as ex:
        db.rollback()
        print(f"[bridge] DB error for uuid={data.get('uuid')}: {ex}")
    finally:
        cursor.close()
        db.close()

def on_connect(client, _userdata, _flags, reason_code, _properties):
    print(f"[bridge] MQTT connected rc={reason_code}")
    # `upload`  = bulk SD-side replay (one msg per experiment).
    # `spectra` = live publish on save (same payload shape).
    # `status`  = heartbeat (logged, not stored).
    # `cal_progress` / `meas_progress` / `monitor` = live frames for the
    # dashboard; the broker fans them out over WebSocket — bridge ignores them.
    client.subscribe("esp32/data/upload")
    client.subscribe("esp32/data/spectra")
    client.subscribe("esp32/data/status")

def on_message(_client, _userdata, msg):
    topic = msg.topic
    try:
        data = json.loads(msg.payload.decode())
    except Exception as ex:
        print(f"[bridge] non-JSON on {topic}: {ex}")
        return
    if topic in ("esp32/data/upload", "esp32/data/spectra"):
        store_experiment(data)
    elif topic == "esp32/data/status":
        print(f"[bridge] heartbeat state={data.get('state')} rssi={data.get('rssi')}")

if __name__ == "__main__":
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_BROKER, MQTT_PORT)
    client.loop_forever()
