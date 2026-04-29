import csv
import io
import json
import os

import flask
import mysql.connector

app = flask.Flask(__name__)

DB_CONFIG = {
    "host":     os.getenv("MYSQL_HOST",     "localhost"),
    "user":     os.getenv("MYSQL_USER",     "root"),
    "password": os.getenv("MYSQL_PASSWORD", ""),
    "database": os.getenv("MYSQL_DATABASE", "espectrografo"),
}

WAVELENGTHS = [410, 435, 460, 485, 510, 535, 560, 585, 610,
               645, 680, 705, 730, 760, 810, 860, 900, 940]
NCH = 18

def get_db():
    return mysql.connector.connect(**DB_CONFIG)

@app.after_request
def add_cors(response):
    response.headers["Access-Control-Allow-Origin"]  = "*"
    response.headers["Access-Control-Allow-Methods"] = "GET,POST,DELETE,OPTIONS"
    response.headers["Access-Control-Allow-Headers"] = "Content-Type"
    return response

@app.route("/")
def serve_html():
    with open("control.html", "r", encoding="utf-8") as f:
        return f.read()

# ─── Experiments listing ────────────────────────────────────────────────────
@app.route("/history/experiments", methods=["GET"])
def get_experiments():
    limit  = min(int(flask.request.args.get("limit", 50)), 500)
    offset = int(flask.request.args.get("offset", 0))
    db = get_db()
    cur = db.cursor(dictionary=True)
    try:
        cur.execute(
            "SELECT * FROM experimentos ORDER BY created_at DESC LIMIT %s OFFSET %s",
            (limit, offset),
        )
        experiments = cur.fetchall()
        cur.execute("SELECT COUNT(*) AS cnt FROM experimentos")
        total = cur.fetchone()["cnt"]
        for e in experiments:
            if e.get("timestamp_ms"):
                e["timestamp_ms"] = int(e["timestamp_ms"])
        return flask.jsonify({"experiments": experiments, "total": total})
    except Exception as ex:
        return flask.jsonify({"error": str(ex)}), 500
    finally:
        cur.close(); db.close()

# ─── Per-experiment fetch (R6: every dataset returns its own calibration) ───
def _fetch_channels(cur, table, uuid):
    cur.execute(
        f"SELECT * FROM {table} WHERE uuid=%s ORDER BY meas_index", (uuid,)
    )
    return [[float(r.get(f"ch{i}", 0) or 0) for i in range(1, NCH + 1)]
            for r in cur.fetchall()]

@app.route("/history/spectra", methods=["GET"])
def get_spectra():
    """Returns Δ-counts + per-experiment calibration + the experiment row.
    The dashboard derives transmittance for live display, but for historical
    plots prefers /history/transmittance and /history/absorbance below."""
    uuid = flask.request.args.get("uuid", "")
    # Backward-compat: accept exp_id when the dashboard hasn't been told
    # about UUIDs yet — pick the most recent experiment with that label.
    if not uuid and flask.request.args.get("exp_id"):
        db = get_db(); cur = db.cursor(dictionary=True)
        cur.execute(
            "SELECT uuid FROM experimentos WHERE exp_id=%s ORDER BY created_at DESC LIMIT 1",
            (flask.request.args.get("exp_id"),),
        )
        row = cur.fetchone(); cur.close(); db.close()
        if row: uuid = row["uuid"]
    if not uuid:
        return flask.jsonify({"error": "uuid (or exp_id) required"}), 400

    db = get_db(); cur = db.cursor(dictionary=True)
    try:
        cur.execute("SELECT * FROM experimentos WHERE uuid=%s", (uuid,))
        exp = cur.fetchone()
        if not exp:
            return flask.jsonify({"error": "not found"}), 404

        cur.execute("SELECT * FROM calibraciones WHERE uuid=%s", (uuid,))
        cal_row = cur.fetchone()
        offsets = ([float(cal_row.get(f"ref_ch{i}", 0) or 0) for i in range(1, NCH + 1)]
                   if cal_row else [])

        spectra = _fetch_channels(cur, "mediciones",     uuid)
        trans   = _fetch_channels(cur, "transmittances", uuid)
        absorb  = _fetch_channels(cur, "absorbancias",   uuid)

        if exp.get("timestamp_ms"):
            exp["timestamp_ms"] = int(exp["timestamp_ms"])

        return flask.jsonify({
            "uuid":            uuid,
            "exp_id":          exp.get("exp_id"),
            "experiment":      exp,           # full config row → drives the table
            "wavelengths":     WAVELENGTHS,
            "offsets":         offsets,
            "spectra":         spectra,       # raw Δ counts
            "transmittance":   trans,         # processed by ESP32, % units
            "absorbance":      absorb,        # processed by ESP32, a.u.
            "num_measurements": len(spectra),
        })
    except Exception as ex:
        return flask.jsonify({"error": str(ex)}), 500
    finally:
        cur.close(); db.close()

# ─── Targeted T / A endpoints ───────────────────────────────────────────────
@app.route("/history/transmittance", methods=["GET"])
def get_transmittance():
    return _table_endpoint("transmittances")

@app.route("/history/absorbance", methods=["GET"])
def get_absorbance():
    return _table_endpoint("absorbancias")

def _table_endpoint(table):
    uuid = flask.request.args.get("uuid", "")
    if not uuid:
        return flask.jsonify({"error": "uuid required"}), 400
    db = get_db(); cur = db.cursor(dictionary=True)
    try:
        rows = _fetch_channels(cur, table, uuid)
        return flask.jsonify({"uuid": uuid, "wavelengths": WAVELENGTHS, "rows": rows})
    finally:
        cur.close(); db.close()

# ─── Manual delete (issue #8.A) ─────────────────────────────────────────────
# ON DELETE CASCADE in the schema removes mediciones/calibraciones/T/A rows.
@app.route("/experiments/<uuid>", methods=["DELETE"])
def delete_experiment(uuid):
    db = get_db(); cur = db.cursor()
    try:
        cur.execute("DELETE FROM experimentos WHERE uuid=%s", (uuid,))
        n = cur.rowcount
        db.commit()
        return flask.jsonify({"deleted": n, "uuid": uuid})
    except Exception as ex:
        db.rollback()
        return flask.jsonify({"error": str(ex)}), 500
    finally:
        cur.close(); db.close()

# ─── Manual import (issue #8.B) ─────────────────────────────────────────────
# Single CSV file matching the device's v3 /spectra.csv schema (one row per
# measurement, calibration inline).  An archive may carry 1..N experiments —
# rows are grouped by `uuid` and inserted as separate experiments.  Each
# experiment goes through the same store_experiment() path the MQTT bridge
# uses, so the dashboard cannot tell device-uploaded from imported.
#
# Required columns (from sd_logger.cpp ensureHeader):
#   uuid, exp_id, date, meas_idx, gain, int_cycles,
#   white_led, white_mA, ir_led, ir_mA, uv_led, uv_mA, n_cal, cal_valid,
#   cal_ch1..cal_ch18, ch1..ch18, t_ch1..t_ch18, a_ch1..a_ch18
@app.route("/experiments/import", methods=["POST"])
def import_experiment():
    upload = (flask.request.files.get("file")
              or flask.request.files.get("spectra")
              or flask.request.files.get("measurements"))
    if not upload:
        return flask.jsonify({"error": "file required (form field 'file')"}), 400

    try:
        rows = list(csv.DictReader(io.StringIO(upload.read().decode("utf-8"))))
    except Exception as ex:
        return flask.jsonify({"error": f"parse failed: {ex}"}), 400
    if not rows:
        return flask.jsonify({"error": "empty file"}), 400

    required_cols = {"uuid", "exp_id", "meas_idx",
                     "gain", "int_cycles",
                     "cal_ch1", "ch1", "t_ch1", "a_ch1"}
    missing = required_cols - set(rows[0].keys())
    if missing:
        return flask.jsonify({
            "error": f"missing columns: {sorted(missing)}",
            "hint": "file must match the device's /spectra.csv v3 schema",
        }), 400

    # Group rows by uuid.  Order within a uuid follows the file order (which
    # the device writes meas_idx-ordered) — we sort defensively in case the
    # user hand-edited the CSV.
    by_uuid = {}
    for r in rows:
        uid = r.get("uuid", "").strip()
        if not uid:
            continue
        by_uuid.setdefault(uid, []).append(r)
    if not by_uuid:
        return flask.jsonify({"error": "no rows with a uuid column"}), 400

    def _floats(row, prefix, n=NCH):
        out = []
        for i in range(1, n + 1):
            v = row.get(f"{prefix}{i}", "")
            try:
                out.append(float(v) if v not in ("", None) else None)
            except ValueError:
                out.append(None)
        return out

    # Gain may be stored either as the AS7265X enum integer (0..3) or as a
    # human label ("16x").  Decode both.
    gain_label_to_int = {"1x": 0, "4x": 1, "16x": 2, "64x": 3}
    def _gain(v):
        s = str(v).strip()
        if s in gain_label_to_int: return gain_label_to_int[s]
        try: return int(s)
        except ValueError: return 0

    def _on(v):
        return str(v).strip().upper() in ("ON", "1", "TRUE", "YES")

    imported, errors = [], []
    from mqtt_to_db import store_experiment
    for uid, group in by_uuid.items():
        try:
            group.sort(key=lambda r: int(r.get("meas_idx", 0) or 0))
            head = group[0]
            payload = {
                "uuid":   uid,
                "exp_id": head.get("exp_id", ""),
                "timestamp_ms": 0,
                "num_measurements": len(group),
                "n_cal": int(head.get("n_cal", 0) or 0),
                "sensor": {
                    "gain":         _gain(head.get("gain", 0)),
                    "mode":         3,
                    "int_cycles":   int(head.get("int_cycles", 0) or 0),
                    "led_white_ma": int(head.get("white_mA", 0) or 0),
                    "led_ir_ma":    int(head.get("ir_mA",    0) or 0),
                    "led_uv_ma":    int(head.get("uv_mA",    0) or 0),
                    "led_white_on": _on(head.get("white_led", "")),
                    "led_ir_on":    _on(head.get("ir_led",    "")),
                    "led_uv_on":    _on(head.get("uv_led",    "")),
                },
                "calibration": {
                    "valid": head.get("cal_valid") in ("1", "true", "True", True, 1),
                    "offsets": _floats(head, "cal_ch"),
                    "cfg_at_cal": {
                        "gain":       _gain(head.get("gain", 0)),
                        "int_cycles": int(head.get("int_cycles", 0) or 0),
                    },
                },
                "spectra":       [_floats(r, "ch")   for r in group],
                "transmittance": [_floats(r, "t_ch") for r in group],
                "absorbance":    [_floats(r, "a_ch") for r in group],
            }
            store_experiment(payload)
            imported.append({"uuid": uid, "exp_id": payload["exp_id"], "rows": len(group)})
        except Exception as ex:
            errors.append({"uuid": uid, "error": str(ex)})

    status = 200 if imported and not errors else (207 if imported else 400)
    return flask.jsonify({"imported": imported, "errors": errors,
                          "total_uuids": len(by_uuid)}), status

# ─── CSV / JSON export (legacy) ─────────────────────────────────────────────
@app.route("/history/export/csv", methods=["GET"])
def export_csv():
    return _export("csv")

@app.route("/history/export/json", methods=["GET"])
def export_json():
    return _export("json")

def _export(fmt):
    uuid     = flask.request.args.get("uuid", "")
    exp_id   = flask.request.args.get("exp_id", "")
    all_data = flask.request.args.get("all", "").lower() == "true"
    if not all_data and not uuid and not exp_id:
        return flask.jsonify({"error": "uuid (or exp_id) required, or all=true"}), 400

    db = get_db(); cur = db.cursor(dictionary=True)
    try:
        if all_data:
            cur.execute("SELECT * FROM experimentos ORDER BY created_at")
        elif uuid:
            cur.execute("SELECT * FROM experimentos WHERE uuid=%s", (uuid,))
        else:
            cur.execute("SELECT * FROM experimentos WHERE exp_id=%s ORDER BY created_at", (exp_id,))
        exps = cur.fetchall()

        if fmt == "csv":
            # v3 layout — round-trippable through /experiments/import.
            # Columns mirror sd_logger.cpp ensureHeader exactly.
            gain_labels = ["1x", "4x", "16x", "64x"]
            out = io.StringIO(); w = csv.writer(out)
            header = (["uuid", "exp_id", "date", "meas_idx",
                       "gain", "int_cycles",
                       "white_led", "white_mA", "ir_led", "ir_mA",
                       "uv_led", "uv_mA", "n_cal", "cal_valid"]
                      + [f"cal_ch{i}" for i in range(1, NCH + 1)]
                      + [f"ch{i}"     for i in range(1, NCH + 1)]
                      + [f"t_ch{i}"   for i in range(1, NCH + 1)]
                      + [f"a_ch{i}"   for i in range(1, NCH + 1)])
            w.writerow(header)
            for e in exps:
                # Pull all four channel blocks for this experiment.
                spectra = _fetch_channels(cur, "mediciones",     e["uuid"])
                trans   = _fetch_channels(cur, "transmittances", e["uuid"])
                absorb  = _fetch_channels(cur, "absorbancias",   e["uuid"])
                cur.execute("SELECT * FROM calibraciones WHERE uuid=%s", (e["uuid"],))
                cal_row = cur.fetchone() or {}
                cal_vec = [float(cal_row.get(f"ref_ch{i}", 0) or 0) for i in range(1, NCH + 1)]
                date = (e.get("created_at") or "").isoformat() if hasattr(e.get("created_at"), "isoformat") \
                       else str(e.get("created_at") or "")
                gain_lbl = gain_labels[e.get("gain", 0)] if 0 <= (e.get("gain") or 0) < 4 else str(e.get("gain"))
                meta_prefix = [
                    e["uuid"], e.get("exp_id"), date,
                    None,               # meas_idx — filled per row below
                    gain_lbl, e.get("int_cycles"),
                    "ON" if e.get("led_white_on") else "OFF", e.get("led_white_ma"),
                    "ON" if e.get("led_ir_on")    else "OFF", e.get("led_ir_ma"),
                    "ON" if e.get("led_uv_on")    else "OFF", e.get("led_uv_ma"),
                    e.get("n_cal"), 1 if e.get("cal_valid") else 0,
                ]
                # Pad missing T/A rows with [None]*NCH so the column count
                # stays constant even if processing failed for some rows.
                pad = lambda lst, idx: lst[idx] if idx < len(lst) else [None] * NCH
                for i, raw in enumerate(spectra):
                    row = list(meta_prefix); row[3] = i
                    row.extend(cal_vec)
                    row.extend(raw)
                    row.extend(pad(trans,  i))
                    row.extend(pad(absorb, i))
                    w.writerow(row)
            resp = flask.Response(out.getvalue(), mimetype="text/csv")
            resp.headers["Content-Disposition"] = 'attachment; filename="export.csv"'
            return resp
        else:
            payload = []
            for e in exps:
                payload.append({
                    "experiment": e,
                    "spectra":       _fetch_channels(cur, "mediciones",     e["uuid"]),
                    "transmittance": _fetch_channels(cur, "transmittances", e["uuid"]),
                    "absorbance":    _fetch_channels(cur, "absorbancias",   e["uuid"]),
                })
            resp = flask.Response(json.dumps(payload, indent=2, default=str),
                                  mimetype="application/json")
            resp.headers["Content-Disposition"] = 'attachment; filename="export.json"'
            return resp
    finally:
        cur.close(); db.close()

# ─── /verify  used by the SD->DB cleanup pass on the ESP32 ──────────────────
@app.route("/verify", methods=["GET"])
def verify_experiment():
    uuid     = flask.request.args.get("uuid", "")
    exp_id   = flask.request.args.get("exp_id", "")
    expected = int(flask.request.args.get("expected", 0) or 0)
    if not uuid and not exp_id:
        return flask.jsonify({"error": "uuid or exp_id required"}), 400

    db = get_db(); cur = db.cursor(dictionary=True)
    try:
        if uuid:
            cur.execute("SELECT COUNT(*) AS cnt FROM mediciones WHERE uuid=%s", (uuid,))
            cnt = int(cur.fetchone()["cnt"])
            cur.execute("SELECT 1 FROM experimentos WHERE uuid=%s", (uuid,))
        else:
            cur.execute("""SELECT COUNT(*) AS cnt FROM mediciones m
                           JOIN experimentos e ON m.uuid=e.uuid
                           WHERE e.exp_id=%s""", (exp_id,))
            cnt = int(cur.fetchone()["cnt"])
            cur.execute("SELECT 1 FROM experimentos WHERE exp_id=%s", (exp_id,))
        registered = cur.fetchone() is not None
        return flask.jsonify({
            "verified":    registered and expected > 0 and cnt >= expected,
            "uuid":        uuid,
            "exp_id":      exp_id,
            "rows_found":  cnt,
            "rows_expected": expected,
            "experiment_registered": registered,
        })
    finally:
        cur.close(); db.close()

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)
