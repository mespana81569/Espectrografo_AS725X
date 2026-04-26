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
# Format MUST match the device's SD layout (clarification #7):
#   - Field `calibration` = the device's /calibrations.csv row for this exp
#     (uuid, exp_id, gain, int_cycles, led_*_ma, led_*_on, ref_ch1..18)
#   - Field `measurements` = the device's /spectra.csv rows for this exp
#     (date, exp_id, uuid, meas_idx, gain, int_cycles, led_*_ma, led_*_on,
#      cal_ch1..18, ch1..18)
# The UI submits both files in one POST; calibration is uploaded FIRST in the
# wizard so the user can confirm the binding before adding the measurements.
@app.route("/experiments/import", methods=["POST"])
def import_experiment():
    cal_file  = flask.request.files.get("calibration")
    meas_file = flask.request.files.get("measurements")
    if not cal_file or not meas_file:
        return flask.jsonify({"error": "both calibration and measurements files required"}), 400

    try:
        cal_rows  = list(csv.DictReader(io.StringIO(cal_file.read().decode("utf-8"))))
        meas_rows = list(csv.DictReader(io.StringIO(meas_file.read().decode("utf-8"))))
    except Exception as ex:
        return flask.jsonify({"error": f"parse failed: {ex}"}), 400

    if not cal_rows or not meas_rows:
        return flask.jsonify({"error": "empty file"}), 400

    # Group by uuid.  Both files MUST agree on uuid; one experiment per import.
    cal_uuids  = {r["uuid"]  for r in cal_rows  if r.get("uuid")}
    meas_uuids = {r["uuid"] for r in meas_rows if r.get("uuid")}
    if len(cal_uuids) != 1 or cal_uuids != meas_uuids:
        return flask.jsonify({
            "error": "uuid mismatch between calibration and measurements files",
            "cal_uuids": sorted(cal_uuids),
            "meas_uuids": sorted(meas_uuids),
        }), 400
    uuid = next(iter(cal_uuids))
    cal = cal_rows[0]
    meas_rows = [r for r in meas_rows if r.get("uuid") == uuid]

    def _floats(row, prefix, n=NCH):
        return [float(row.get(f"{prefix}{i}", 0) or 0) for i in range(1, n + 1)]

    payload = {
        "uuid":   uuid,
        "exp_id": cal.get("exp_id", ""),
        "timestamp_ms": int(meas_rows[0].get("timestamp_ms", 0) or 0),
        "num_measurements": len(meas_rows),
        "n_cal":  int(cal.get("n_cal", 5) or 5),
        "sensor": {
            "gain":       int(cal.get("gain_int", 0) or 0),
            "mode":       int(cal.get("mode", 3) or 3),
            "int_cycles": int(cal.get("int_cycles", 0) or 0),
            "led_white_ma": int(cal.get("led_white_ma", 0) or 0),
            "led_ir_ma":    int(cal.get("led_ir_ma", 0) or 0),
            "led_uv_ma":    int(cal.get("led_uv_ma", 0) or 0),
            "led_white_on": cal.get("led_white_on") in ("ON", "1", "true", True),
            "led_ir_on":    cal.get("led_ir_on")    in ("ON", "1", "true", True),
            "led_uv_on":    cal.get("led_uv_on")    in ("ON", "1", "true", True),
        },
        "calibration": {
            "valid":    cal.get("cal_valid") in ("1", "true", "True", True),
            "offsets":  _floats(cal, "ref_ch"),
            "cfg_at_cal": {
                "gain":       int(cal.get("gain_int", 0) or 0),
                "int_cycles": int(cal.get("int_cycles", 0) or 0),
            },
        },
        "spectra":       [_floats(r, "ch") for r in meas_rows],
        # T / A are recomputed below from the imported raw counts so we
        # don't trust the file to have them.
    }

    offsets = payload["calibration"]["offsets"]
    def _T(row):
        out = []
        for i, d in enumerate(row):
            I0 = offsets[i] if i < len(offsets) else 0
            if I0 <= 0:
                out.append(None); continue
            I = d + I0
            if I < 0: I = 0
            out.append(I / I0 * 100.0)
        return out
    def _A(t_row):
        out = []
        for t in t_row:
            if t is None or t <= 0:
                out.append(None)
            else:
                import math
                out.append(-math.log10(t / 100.0))
        return out
    payload["transmittance"] = [_T(r) for r in payload["spectra"]]
    payload["absorbance"]    = [_A(t) for t in payload["transmittance"]]

    # Reuse the bridge code path for actual insert.  Importing here keeps the
    # import response identical to a device-uploaded experiment.
    from mqtt_to_db import store_experiment
    store_experiment(payload)
    return flask.jsonify({"imported": uuid, "rows": len(meas_rows)})

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
            out = io.StringIO(); w = csv.writer(out)
            w.writerow(["uuid", "exp_id", "meas_index", "timestamp_ms",
                        "gain", "int_cycles"] + [f"ch{i}" for i in range(1, NCH + 1)])
            for e in exps:
                rows = _fetch_channels(cur, "mediciones", e["uuid"])
                for i, row in enumerate(rows):
                    w.writerow([e["uuid"], e.get("exp_id"), i, int(e.get("timestamp_ms") or 0),
                                e.get("gain"), e.get("int_cycles")] + row)
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
