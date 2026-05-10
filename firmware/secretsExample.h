#pragma once
// Template — copy to firmware/secrets.h (gitignored) and fill in real values.
// HOST is shared by MQTT broker (port 1883) and Flask /verify (port 5000),
// so it should be the public IP / domain of the docker host.
//
// MQTT_USERNAME / MQTT_PASSWORD must match the entry in docker/mosquitto/passwd.
// FLASK_API_KEY must match docker/.env -> API_KEY.

#define HOST              "0.0.0.0"
#define DB_VERIFY_PORT    5000
#define MQTT_USERNAME     "usuario_mqtt"
#define MQTT_PASSWORD     "XXXXXXXX"
#define FLASK_API_KEY     "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
