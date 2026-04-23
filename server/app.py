import flask
import mysql.connector
import json
import csv
import io
import os

app = flask.Flask(__name__)

DB_CONFIG = {
    "host":     os.getenv("MYSQL_HOST",     "localhost"),
    "user":     os.getenv("MYSQL_USER",     "root"),
    "password": os.getenv("MYSQL_PASSWORD", ""),
    "database": os.getenv("MYSQL_DATABASE", "espectrografo")
}

WAVELENGTHS = [410,435,460,485,510,535,560,585,610,645,680,705,730,760,810,860,900,940]

def get_db():
    return mysql.connector.connect(**DB_CONFIG)

@app.after_request
def add_cors(response):
    response.headers['Access-Control-Allow-Origin']  = '*'
    response.headers['Access-Control-Allow-Methods'] = 'GET,POST,OPTIONS'
    response.headers['Access-Control-Allow-Headers'] = 'Content-Type'
    return response

@app.route('/')
def serve_html():
    with open('control.html', 'r', encoding='utf-8') as f:
        return f.read()

@app.route('/history/experiments', methods=['GET'])
def get_experiments():
    limit  = min(int(flask.request.args.get('limit', 50)), 500)
    offset = int(flask.request.args.get('offset', 0))
    db = get_db()
    cursor = db.cursor(dictionary=True)
    try:
        cursor.execute(
            'SELECT * FROM experimentos ORDER BY timestamp_ms DESC LIMIT %s OFFSET %s',
            (limit, offset)
        )
        experiments = cursor.fetchall()
        cursor.execute('SELECT COUNT(*) as cnt FROM experimentos')
        total = cursor.fetchone()['cnt']
        for e in experiments:
            if e.get('timestamp_ms'):
                e['timestamp_ms'] = int(e['timestamp_ms'])
        return flask.jsonify({'experiments': experiments, 'total': total})
    except Exception as ex:
        return flask.jsonify({'error': str(ex)}), 500
    finally:
        cursor.close(); db.close()

@app.route('/history/spectra', methods=['GET'])
def get_spectra():
    exp_id = flask.request.args.get('exp_id', '')
    if not exp_id:
        return flask.jsonify({'error': 'exp_id required'}), 400
    db = get_db()
    cursor = db.cursor(dictionary=True)
    try:
        cursor.execute('SELECT * FROM mediciones WHERE exp_id=%s ORDER BY meas_index', (exp_id,))
        meas_rows = cursor.fetchall()
        cursor.execute('SELECT * FROM calibraciones WHERE exp_id=%s', (exp_id,))
        cal_row = cursor.fetchone()
        cursor.execute('SELECT timestamp_ms FROM experimentos WHERE exp_id=%s', (exp_id,))
        exp_row = cursor.fetchone()
        spectra = [[float(r.get(f'ch{i}',0)) for i in range(1,19)] for r in meas_rows]
        offsets = [float(cal_row.get(f'ch{i}',0)) for i in range(1,19)] if cal_row else []
        return flask.jsonify({
            'exp_id': exp_id,
            'timestamp_ms': int(exp_row['timestamp_ms']) if exp_row else 0,
            'spectra': spectra, 'offsets': offsets,
            'wavelengths': WAVELENGTHS, 'num_measurements': len(spectra)
        })
    except Exception as ex:
        return flask.jsonify({'error': str(ex)}), 500
    finally:
        cursor.close(); db.close()

@app.route('/history/export/csv', methods=['GET'])
def export_csv():
    exp_id   = flask.request.args.get('exp_id', '')
    all_data = flask.request.args.get('all','').lower() == 'true'
    if not all_data and not exp_id:
        return flask.jsonify({'error': 'exp_id required or all=true'}), 400
    db = get_db()
    cursor = db.cursor(dictionary=True)
    try:
        output = io.StringIO()
        writer = csv.writer(output)
        writer.writerow(['exp_id','meas_index','timestamp_ms','gain','mode','int_cycles',
                         'led_white_ma','led_ir_ma','led_uv_ma'] + [f'ch{i}' for i in range(1,19)])
        q = '''SELECT m.*, e.timestamp_ms, e.gain, e.mode, e.int_cycles,
                      e.led_white_ma, e.led_ir_ma, e.led_uv_ma
               FROM mediciones m JOIN experimentos e ON m.exp_id=e.exp_id'''
        cursor.execute(q if all_data else q+' WHERE m.exp_id=%s ORDER BY m.meas_index',
                       () if all_data else (exp_id,))
        for row in cursor.fetchall():
            writer.writerow([row['exp_id'], row['meas_index'],
                             int(row['timestamp_ms'] or 0),
                             row['gain'], row['mode'], row['int_cycles'],
                             row['led_white_ma'], row['led_ir_ma'], row['led_uv_ma']]
                            + [float(row.get(f'ch{i}',0)) for i in range(1,19)])
        fname = 'all_spectra.csv' if all_data else f'{exp_id}.csv'
        resp = flask.Response(output.getvalue(), mimetype='text/csv')
        resp.headers['Content-Disposition'] = f'attachment; filename="{fname}"'
        return resp
    except Exception as ex:
        return flask.jsonify({'error': str(ex)}), 500
    finally:
        cursor.close(); db.close()

@app.route('/history/export/json', methods=['GET'])
def export_json():
    exp_id   = flask.request.args.get('exp_id', '')
    all_data = flask.request.args.get('all','').lower() == 'true'
    if not all_data and not exp_id:
        return flask.jsonify({'error': 'exp_id required or all=true'}), 400
    db = get_db()
    cursor = db.cursor(dictionary=True)
    try:
        cursor.execute('SELECT * FROM experimentos' if all_data
                       else 'SELECT * FROM experimentos WHERE exp_id=%s',
                       () if all_data else (exp_id,))
        result = []
        for exp in cursor.fetchall():
            cursor.execute('SELECT * FROM mediciones WHERE exp_id=%s ORDER BY meas_index', (exp['exp_id'],))
            spectra = [[float(r.get(f'ch{i}',0)) for i in range(1,19)] for r in cursor.fetchall()]
            cursor.execute('SELECT * FROM calibraciones WHERE exp_id=%s', (exp['exp_id'],))
            cal = cursor.fetchone()
            offsets = [float(cal.get(f'ch{i}',0)) for i in range(1,19)] if cal else []
            result.append({
                'exp_id': exp['exp_id'], 'timestamp_ms': int(exp['timestamp_ms'] or 0),
                'num_measurements': len(spectra), 'gain': exp['gain'],
                'mode': exp['mode'], 'int_cycles': exp['int_cycles'],
                'led_white_ma': exp['led_white_ma'], 'led_ir_ma': exp['led_ir_ma'],
                'led_uv_ma': exp['led_uv_ma'], 'cal_valid': exp['cal_valid'],
                'spectra': spectra, 'calibration_offsets': offsets, 'wavelengths': WAVELENGTHS
            })
        fname = 'all_spectra.json' if all_data else f'{exp_id}.json'
        resp = flask.Response(json.dumps(result, indent=2), mimetype='application/json')
        resp.headers['Content-Disposition'] = f'attachment; filename="{fname}"'
        return resp
    except Exception as ex:
        return flask.jsonify({'error': str(ex)}), 500
    finally:
        cursor.close(); db.close()

@app.route('/verify', methods=['GET'])
def verify_experiment():
    exp_id = flask.request.args.get('exp_id','').strip()
    try:
        expected = int(flask.request.args.get('expected', 0))
    except ValueError:
        expected = 0
    if not exp_id:
        return flask.jsonify({'error': 'exp_id required'}), 400
    db = get_db()
    cursor = db.cursor(dictionary=True)
    try:
        cursor.execute('SELECT COUNT(*) AS cnt FROM mediciones WHERE exp_id=%s', (exp_id,))
        rows_found = int(cursor.fetchone()['cnt'])
        cursor.execute('SELECT 1 FROM experimentos WHERE exp_id=%s LIMIT 1', (exp_id,))
        exp_exists = cursor.fetchone() is not None
        return flask.jsonify({
            'verified': exp_exists and expected > 0 and rows_found >= expected,
            'exp_id': exp_id, 'rows_found': rows_found,
            'rows_expected': expected, 'experiment_registered': exp_exists
        })
    except Exception as ex:
        return flask.jsonify({'error': str(ex)}), 500
    finally:
        cursor.close(); db.close()

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, debug=False)