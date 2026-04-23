import paho.mqtt.client as mqtt
import mysql.connector
import json
import os

# Configuración desde variables de entorno (Docker) o defaults locales
MQTT_BROKER = os.getenv("MQTT_BROKER", "localhost")
MQTT_PORT   = int(os.getenv("MQTT_PORT", "1884"))
DB_CONFIG = {
    "host":     os.getenv("MYSQL_HOST",     "localhost"),
    "user":     os.getenv("MYSQL_USER",     "root"),
    "password": os.getenv("MYSQL_PASSWORD", ""),
    "database": os.getenv("MYSQL_DATABASE", "espectrografo")
}

def get_db():
    return mysql.connector.connect(**DB_CONFIG)

def on_connect(client, userdata, flags, reason_code, properties):
    print(f"Conectado al broker MQTT rc={reason_code}")
    client.subscribe("esp32/data/upload")
    client.subscribe("esp32/data/spectra")
    client.subscribe("esp32/data/status")

def on_message(client, userdata, msg):
    topic = msg.topic
    try:
        data = json.loads(msg.payload.decode())
        if topic in ["esp32/data/upload", "esp32/data/spectra"]:
            print(f"Recibido experimento en {topic}: exp_id={data.get('exp_id')}")
            guardar_experimento(data)
        else:
            print(f"Heartbeat: state={data.get('state')} rssi={data.get('rssi')}")
    except Exception as e:
        print(f"Error procesando mensaje: {e}")

def guardar_experimento(data):
    db = get_db()
    cursor = db.cursor()
    try:
        sensor  = data.get("sensor", {})
        cal     = data.get("calibration", {})
        offsets = cal.get("offsets", [0]*18)

        cursor.execute("""
            INSERT IGNORE INTO experimentos
            (exp_id, timestamp_ms, num_measurements, gain, mode, int_cycles,
             led_white_ma, led_ir_ma, led_uv_ma, cal_valid)
            VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)
        """, (
            data["exp_id"],
            data.get("timestamp_ms", 0),
            data.get("num_measurements", 0),
            sensor.get("gain", 0),
            sensor.get("mode", 0),
            sensor.get("int_cycles", 0),
            sensor.get("led_white_ma", 0),
            sensor.get("led_ir_ma", 0),
            sensor.get("led_uv_ma", 0),
            cal.get("valid", False)
        ))

        cursor.execute("""
            INSERT IGNORE INTO calibraciones
            (exp_id,ch1,ch2,ch3,ch4,ch5,ch6,ch7,ch8,ch9,
             ch10,ch11,ch12,ch13,ch14,ch15,ch16,ch17,ch18)
            VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)
        """, (data["exp_id"], *offsets[:18]))

        for i, espectro in enumerate(data.get("spectra", [])):
            cursor.execute("""
                INSERT INTO mediciones
                (exp_id,meas_index,ch1,ch2,ch3,ch4,ch5,ch6,ch7,ch8,ch9,
                 ch10,ch11,ch12,ch13,ch14,ch15,ch16,ch17,ch18)
                VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)
            """, (data["exp_id"], i, *espectro[:18]))

        db.commit()
        print(f"Experimento {data['exp_id']} guardado correctamente")

    except Exception as e:
        db.rollback()
        print(f"Error guardando en DB: {e}")
    finally:
        cursor.close()
        db.close()

# Arrancar cliente MQTT con API v2
client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = on_connect
client.on_message = on_message
client.connect(MQTT_BROKER, MQTT_PORT)
client.loop_forever()
