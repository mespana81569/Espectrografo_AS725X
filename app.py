import flask
import mysql.connector
import json
import csv
import io
import os
from datetime import datetime

app = flask.Flask(__name__)

DB_CONFIG = {
    "host": "localhost",
    "user": "root",
    "password": "",
    "database": "espectrografo"
}

WAVELENGTHS = [410, 435, 460, 485, 510, 535, 560, 585, 610, 645, 680, 705, 730, 760, 810, 860, 900, 940]

def get_db():
    return mysql.connector.connect(**DB_CONFIG)

@app.before_request
def cors_headers():
    flask.response.headers['Access-Control-Allow-Origin'] = '*'
    flask.response.headers['Access-Control-Allow-Methods'] = 'GET,POST,OPTIONS'
    flask.response.headers['Access-Control-Allow-Headers'] = 'Content-Type'

@app.route('/')
def serve_html():
    with open('control.html', 'r', encoding='utf-8') as f:
        return f.read()

@app.route('/history/experiments', methods=['GET'])
def get_experiments():
    limit = min(int(flask.request.args.get('limit', 50)), 500)
    offset = int(flask.request.args.get('offset', 0))

    db = get_db()
    cursor = db.cursor(dictionary=True)

    try:
        cursor.execute(
            "SELECT * FROM experimentos ORDER BY timestamp_ms DESC LIMIT %s OFFSET %s",
            (limit, offset)
        )
        experiments = cursor.fetchall()

        cursor.execute("SELECT COUNT(*) as cnt FROM experimentos")
        total = cursor.fetchone()['cnt']

        for e in experiments:
            if e.get('timestamp_ms'):
                e['timestamp_ms'] = int(e['timestamp_ms'])

        return flask.jsonify({"experiments": experiments, "total": total})
    except Exception as ex:
        return flask.jsonify({"error": str(ex)}), 500
    finally:
        cursor.close()
        db.close()

@app.route('/history/spectra', methods=['GET'])
def get_spectra():
    exp_id = flask.request.args.get('exp_id', '')
    if not exp_id:
        return flask.jsonify({"error": "exp_id required"}), 400

    db = get_db()
    cursor = db.cursor(dictionary=True)

    try:
        cursor.execute(
            "SELECT * FROM mediciones WHERE exp_id = %s ORDER BY meas_index ASC",
            (exp_id,)
        )
        meas_rows = cursor.fetchall()

        cursor.execute(
            "SELECT * FROM calibraciones WHERE exp_id = %s",
            (exp_id,)
        )
        cal_row = cursor.fetchone()

        cursor.execute(
            "SELECT timestamp_ms FROM experimentos WHERE exp_id = %s",
            (exp_id,)
        )
        exp_row = cursor.fetchone()

        spectra = []
        for row in meas_rows:
            spec = [float(row.get(f'ch{i}', 0)) for i in range(1, 19)]
            spectra.append(spec)

        offsets = []
        if cal_row:
            offsets = [float(cal_row.get(f'ch{i}', 0)) for i in range(1, 19)]

        return flask.jsonify({
            "exp_id": exp_id,
            "timestamp_ms": int(exp_row['timestamp_ms']) if exp_row else 0,
            "spectra": spectra,
            "offsets": offsets,
            "wavelengths": WAVELENGTHS,
            "num_measurements": len(spectra)
        })
    except Exception as ex:
        return flask.jsonify({"error": str(ex)}), 500
    finally:
        cursor.close()
        db.close()

@app.route('/history/export/csv', methods=['GET'])
def export_csv():
    exp_id = flask.request.args.get('exp_id', '')
    all_data = flask.request.args.get('all', '').lower() == 'true'

    if not all_data and not exp_id:
        return flask.jsonify({"error": "exp_id required or all=true"}), 400

    db = get_db()
    cursor = db.cursor(dictionary=True)

    try:
        output = io.StringIO()
        writer = csv.writer(output)

        header = ['exp_id', 'meas_index', 'timestamp_ms', 'gain', 'mode', 'int_cycles',
                  'led_white_ma', 'led_ir_ma', 'led_uv_ma']
        header.extend([f'ch{i}' for i in range(1, 19)])
        writer.writerow(header)

        if all_data:
            cursor.execute("""
                SELECT m.*, e.timestamp_ms, e.gain, e.mode, e.int_cycles,
                       e.led_white_ma, e.led_ir_ma, e.led_uv_ma
                FROM mediciones m
                JOIN experimentos e ON m.exp_id = e.exp_id
                ORDER BY e.timestamp_ms DESC, m.meas_index ASC
            """)
        else:
            cursor.execute("""
                SELECT m.*, e.timestamp_ms, e.gain, e.mode, e.int_cycles,
                       e.led_white_ma, e.led_ir_ma, e.led_uv_ma
                FROM mediciones m
                JOIN experimentos e ON m.exp_id = e.exp_id
                WHERE m.exp_id = %s
                ORDER BY m.meas_index ASC
            """, (exp_id,))

        rows = cursor.fetchall()
        for row in rows:
            csv_row = [
                row['exp_id'],
                row['meas_index'],
                int(row['timestamp_ms']) if row['timestamp_ms'] else 0,
                row['gain'],
                row['mode'],
                row['int_cycles'],
                row['led_white_ma'],
                row['led_ir_ma'],
                row['led_uv_ma']
            ]
            csv_row.extend([float(row.get(f'ch{i}', 0)) for i in range(1, 19)])
            writer.writerow(csv_row)

        csv_data = output.getvalue()

        fname = 'all_spectra.csv' if all_data else f'{exp_id}.csv'
        resp = flask.Response(csv_data, mimetype='text/csv')
        resp.headers['Content-Disposition'] = f'attachment; filename="{fname}"'
        return resp
    except Exception as ex:
        return flask.jsonify({"error": str(ex)}), 500
    finally:
        cursor.close()
        db.close()

@app.route('/verify', methods=['GET'])
def verify_experiment():
    exp_id = flask.request.args.get('exp_id', '').strip()
    try:
        expected = int(flask.request.args.get('expected', '0'))
    except ValueError:
        expected = 0

    if not exp_id:
        return flask.jsonify({"error": "exp_id required"}), 400

    db = get_db()
    cursor = db.cursor(dictionary=True)

    try:
        cursor.execute(
            "SELECT COUNT(*) AS cnt FROM mediciones WHERE exp_id = %s",
            (exp_id,)
        )
        row = cursor.fetchone()
        rows_found = int(row['cnt']) if row else 0

        cursor.execute(
            "SELECT 1 FROM experimentos WHERE exp_id = %s LIMIT 1",
            (exp_id,)
        )
        exp_exists = cursor.fetchone() is not None

        verified = exp_exists and expected > 0 and rows_found >= expected

        return flask.jsonify({
            "verified": verified,
            "exp_id": exp_id,
            "rows_found": rows_found,
            "rows_expected": expected,
            "experiment_registered": exp_exists
        })
    except Exception as ex:
        return flask.jsonify({"error": str(ex)}), 500
    finally:
        cursor.close()
        db.close()

@app.route('/history/export/json', methods=['GET'])
def export_json():
    exp_id = flask.request.args.get('exp_id', '')
    all_data = flask.request.args.get('all', '').lower() == 'true'

    if not all_data and not exp_id:
        return flask.jsonify({"error": "exp_id required or all=true"}), 400

    db = get_db()
    cursor = db.cursor(dictionary=True)

    try:
        if all_data:
            cursor.execute("SELECT * FROM experimentos ORDER BY timestamp_ms DESC")
        else:
            cursor.execute("SELECT * FROM experimentos WHERE exp_id = %s", (exp_id,))

        exp_rows = cursor.fetchall()

        result_list = []
        for exp in exp_rows:
            cursor.execute(
                "SELECT * FROM mediciones WHERE exp_id = %s ORDER BY meas_index ASC",
                (exp['exp_id'],)
            )
            meas_rows = cursor.fetchall()

            cursor.execute(
                "SELECT * FROM calibraciones WHERE exp_id = %s",
                (exp['exp_id'],)
            )
            cal_row = cursor.fetchone()

            spectra = []
            for row in meas_rows:
                spec = [float(row.get(f'ch{i}', 0)) for i in range(1, 19)]
                spectra.append(spec)

            offsets = []
            if cal_row:
                offsets = [float(cal_row.get(f'ch{i}', 0)) for i in range(1, 19)]

            exp['timestamp_ms'] = int(exp['timestamp_ms'])
            result_list.append({
                "exp_id": exp['exp_id'],
                "timestamp_ms": exp['timestamp_ms'],
                "num_measurements": len(spectra),
                "gain": exp['gain'],
                "mode": exp['mode'],
                "int_cycles": exp['int_cycles'],
                "led_white_ma": exp['led_white_ma'],
                "led_ir_ma": exp['led_ir_ma'],
                "led_uv_ma": exp['led_uv_ma'],
                "cal_valid": exp['cal_valid'],
                "spectra": spectra,
                "calibration_offsets": offsets,
                "wavelengths": WAVELENGTHS
            })

        json_data = json.dumps(result_list, indent=2)

        fname = 'all_spectra.json' if all_data else f'{exp_id}.json'
        resp = flask.Response(json_data, mimetype='application/json')
        resp.headers['Content-Disposition'] = f'attachment; filename="{fname}"'
        return resp
    except Exception as ex:
        return flask.jsonify({"error": str(ex)}), 500
    finally:
        cursor.close()
        db.close()

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, debug=False)
