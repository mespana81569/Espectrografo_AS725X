import csv
import io
import json
import os
import secrets
from datetime import timedelta
from functools import wraps

import flask
import mysql.connector
from markupsafe import escape

app = flask.Flask(__name__)

# ─── Auth configuration ─────────────────────────────────────────────────────
# Session-cookie auth gates the dashboard.  The ESP32 keeps its own token-only
# path on /verify since it cannot hold a cookie across reboots.
LOGIN_USERNAME    = os.getenv("LOGIN_USERNAME",    "")
LOGIN_PASSWORD    = os.getenv("LOGIN_PASSWORD",    "")
FLASK_SECRET_KEY  = os.getenv("FLASK_SECRET_KEY",  "")
SESSION_HOURS     = int(os.getenv("SESSION_HOURS", "12"))
COOKIE_SECURE     = os.getenv("SESSION_COOKIE_SECURE", "0") == "1"

# /verify (ESP32 only) — kept distinct in name to make the eventual split into
# DEVICE_VERIFY_TOKEN obvious.  Today it equals the value the firmware already
# carries in mqtt_credentials.h (FLASK_API_KEY).
API_KEY = os.getenv("API_KEY", "")

# Fail loud, not silent — a missing secret_key produces working logins that all
# share the same default-empty signing key, which would let an attacker forge a
# session cookie.
if not FLASK_SECRET_KEY:
    raise RuntimeError(
        "FLASK_SECRET_KEY is not set. Generate one (e.g. `python -c "
        "\"import secrets; print(secrets.token_hex(32))\"`) and put it in docker/.env"
    )
if not LOGIN_USERNAME or not LOGIN_PASSWORD:
    raise RuntimeError(
        "LOGIN_USERNAME and LOGIN_PASSWORD must be set in docker/.env "
        "before the dashboard can start."
    )

app.secret_key = FLASK_SECRET_KEY
app.permanent_session_lifetime = timedelta(hours=SESSION_HOURS)
app.config.update(
    SESSION_COOKIE_HTTPONLY = True,
    SESSION_COOKIE_SAMESITE = "Lax",   # allows top-level GET nav, blocks cross-site POST
    SESSION_COOKIE_SECURE   = COOKIE_SECURE,
)

def _is_logged_in():
    return flask.session.get("user") == LOGIN_USERNAME

def require_login(f):
    @wraps(f)
    def decorated(*args, **kwargs):
        if not _is_logged_in():
            # XHR (fetch) gets JSON 401 — the dashboard's apiFetch wrapper
            # turns that into a redirect to /login.  Plain navigation
            # (window.location, <a href>, file downloads) gets a 302 so the
            # browser lands on the login page directly.
            accept = flask.request.headers.get("Accept", "")
            wants_html = "text/html" in accept and "application/json" not in accept
            if wants_html:
                return flask.redirect(flask.url_for("login_page"))
            return flask.jsonify({"error": "unauthorized"}), 401
        return f(*args, **kwargs)
    return decorated

DB_CONFIG = {
    "host":     os.getenv("MYSQL_HOST",     "localhost"),
    "user":     os.getenv("MYSQL_USER",     "root"),
    "password": os.getenv("MYSQL_PASSWORD", ""),
    "database": os.getenv("MYSQL_DATABASE", "espectrografo"),
}

# Where the browser must reach the broker over WebSockets.  Templated into
# control.html at /; not a secret, just deploy-specific.
MQTT_PUBLIC_HOST    = os.getenv("MQTT_PUBLIC_HOST",    "localhost")
MQTT_PUBLIC_WS_PORT = int(os.getenv("MQTT_PUBLIC_WS_PORT", "9001"))

WAVELENGTHS = [410, 435, 460, 485, 510, 535, 560, 585, 610,
               645, 680, 705, 730, 760, 810, 860, 900, 940]
NCH = 18

def get_db():
    return mysql.connector.connect(**DB_CONFIG)

@app.after_request
def add_cors(response):
    # Cookie-auth is same-origin only (we don't set Allow-Credentials), so
    # this header only matters for cross-origin GETs to public endpoints.
    response.headers["Access-Control-Allow-Origin"]  = "*"
    response.headers["Access-Control-Allow-Methods"] = "GET,POST,DELETE,OPTIONS"
    response.headers["Access-Control-Allow-Headers"] = "Content-Type"
    return response

# ─── Auth pages ─────────────────────────────────────────────────────────────

LOGIN_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>Sign in &mdash; Spectrograph</title>
<style>
:root{--bg:#0f172a;--card:#1e293b;--bdr:#334155;--accent:#38bdf8;
  --text:#e2e8f0;--muted:#94a3b8;--danger:#f87171}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',system-ui,sans-serif;background:var(--bg);color:var(--text);
  min-height:100vh;display:flex;align-items:center;justify-content:center;padding:1rem}
.card{background:var(--card);border:1px solid var(--bdr);border-radius:.7rem;
  padding:1.5rem 1.5rem 1.7rem;width:100%;max-width:340px}
h1{color:var(--accent);font-size:1.05rem;margin-bottom:1rem;text-align:center;letter-spacing:.05em}
label{display:block;font-size:.72rem;color:var(--muted);margin:.65rem 0 .2rem}
input{width:100%;padding:.45rem .55rem;background:var(--bg);border:1px solid var(--bdr);
  border-radius:.3rem;color:var(--text);font-size:.85rem;font-family:inherit}
input:focus{outline:none;border-color:var(--accent)}
button{width:100%;padding:.55rem;margin-top:1.1rem;border:none;border-radius:.3rem;
  background:var(--accent);color:var(--bg);font-size:.82rem;font-weight:700;cursor:pointer;
  letter-spacing:.04em}
button:hover{opacity:.85}
.err{margin-top:.85rem;padding:.4rem;background:var(--danger);color:var(--bg);
  border-radius:.3rem;font-size:.72rem;text-align:center;font-weight:600}
.muted{margin-top:.9rem;color:var(--muted);font-size:.65rem;text-align:center}
</style>
</head>
<body>
<form class="card" method="POST" action="/login">
  <h1>Spectrograph &mdash; Sign in</h1>
  <label for="u">Username</label>
  <input id="u" name="username" autocomplete="username" required autofocus/>
  <label for="p">Password</label>
  <input id="p" name="password" type="password" autocomplete="current-password" required/>
  <button type="submit">Sign in</button>
  __ERROR__
  <div class="muted">AS7265X spectrograph &middot; restricted access</div>
</form>
</body>
</html>"""

def _render_login(error=None):
    block = (f'<div class="err">{escape(error)}</div>'
             if error else "")
    return LOGIN_HTML.replace("__ERROR__", block)

@app.route("/login", methods=["GET"])
def login_page():
    if _is_logged_in():
        return flask.redirect("/")
    return _render_login()

@app.route("/login", methods=["POST"])
def login_submit():
    u = (flask.request.form.get("username") or "").strip()
    p = flask.request.form.get("password") or ""
    # Constant-time compare on both fields so a wrong username and a wrong
    # password take the same time — denies username enumeration via timing.
    ok_u = secrets.compare_digest(u.encode(),       LOGIN_USERNAME.encode())
    ok_p = secrets.compare_digest(p.encode(),       LOGIN_PASSWORD.encode())
    if ok_u and ok_p:
        flask.session.clear()                # rotate session id on auth success
        flask.session["user"] = LOGIN_USERNAME
        flask.session.permanent = True       # honour permanent_session_lifetime
        return flask.redirect("/")
    return _render_login("Invalid credentials"), 401

@app.route("/logout", methods=["POST", "GET"])
def logout():
    flask.session.clear()
    return flask.redirect(flask.url_for("login_page"))

# ─── Dashboard ──────────────────────────────────────────────────────────────

@app.route("/")
def serve_html():
    if not _is_logged_in():
        return flask.redirect(flask.url_for("login_page"))
    with open("control.html", "r", encoding="utf-8") as f:
        html = f.read()
    # Inject deploy-time broker host/port so control.html stays generic.
    # Both literals must match the source exactly — see lines 329-330 there.
    html = html.replace(
        'var MQTT_HOST = "localhost";',
        f'var MQTT_HOST = {json.dumps(MQTT_PUBLIC_HOST)};',
    ).replace(
        'var MQTT_PORT = 9001;',
        f'var MQTT_PORT = {MQTT_PUBLIC_WS_PORT};',
    )
    return html

@app.route("/history/experiments", methods=["GET"])
@require_login
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

def _fetch_channels(cur, table, uuid):
    cur.execute(
        f"SELECT * FROM {table} WHERE uuid=%s ORDER BY meas_index", (uuid,)
    )
    return [[float(r.get(f"ch{i}", 0) or 0) for i in range(1, NCH + 1)]
            for r in cur.fetchall()]

@app.route("/history/spectra", methods=["GET"])
@require_login
def get_spectra():
    uuid = flask.request.args.get("uuid", "")
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
            "uuid":             uuid,
            "exp_id":           exp.get("exp_id"),
            "experiment":       exp,
            "wavelengths":      WAVELENGTHS,
            "offsets":          offsets,
            "spectra":          spectra,
            "transmittance":    trans,
            "absorbance":       absorb,
            "num_measurements": len(spectra),
        })
    except Exception as ex:
        return flask.jsonify({"error": str(ex)}), 500
    finally:
        cur.close(); db.close()

@app.route("/history/transmittance", methods=["GET"])
@require_login
def get_transmittance():
    return _table_endpoint("transmittances")

@app.route("/history/absorbance", methods=["GET"])
@require_login
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

@app.route("/experiments/<uuid>", methods=["DELETE"])
@require_login
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

@app.route("/experiments/import", methods=["POST"])
@require_login
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

@app.route("/history/export/csv", methods=["GET"])
@require_login
def export_csv():
    return _export("csv")

@app.route("/history/export/json", methods=["GET"])
@require_login
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
                    None,
                    gain_lbl, e.get("int_cycles"),
                    "ON" if e.get("led_white_on") else "OFF", e.get("led_white_ma"),
                    "ON" if e.get("led_ir_on")    else "OFF", e.get("led_ir_ma"),
                    "ON" if e.get("led_uv_on")    else "OFF", e.get("led_uv_ma"),
                    e.get("n_cal"), 1 if e.get("cal_valid") else 0,
                ]
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

@app.route("/verify", methods=["GET"])
def verify_experiment():
    token = flask.request.args.get("token", "")
    if API_KEY and token != API_KEY:
        return flask.jsonify({"error": "unauthorized"}), 401

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
            "verified":              registered and expected > 0 and cnt >= expected,
            "uuid":                  uuid,
            "exp_id":                exp_id,
            "rows_found":            cnt,
            "rows_expected":         expected,
            "experiment_registered": registered,
        })
    finally:
        cur.close(); db.close()

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)