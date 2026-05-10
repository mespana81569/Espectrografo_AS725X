# Spectrograph AS7265X

ESP32 portable water-analysis spectrograph using the AS7265X 18-channel spectral
sensor (410–940 nm). Firmware runs a state machine with a local HTTP UI
(`192.168.4.1`) and an MQTT mirror to a remote dashboard. Spectra are persisted
to microSD (CSV v3, 86 columns), bulk-uploaded over MQTT, and verified against
a MySQL database before being purged from the device.

See [CLAUDE.md](CLAUDE.md) for the full architecture overview.

## Layout

```
firmware/        PlatformIO project (esp32doit-devkit-v1)
server/          Flask dashboard + MQTT→MySQL bridge
docker/          docker-compose stack (mosquitto, mysql, flask, bridge)
```

## Deploying from scratch

The four-container stack runs on a public Linux host (production: 1 vCPU /
512 MB DigitalOcean droplet, Ubuntu 24.04). On a fresh host:

```bash
# 1. Clone
git clone <repo-url> espectrografo && cd espectrografo

# 2. Create swap (skip if RAM ≥ 2 GB)
sudo fallocate -l 1G /swapfile && sudo chmod 600 /swapfile
sudo mkswap /swapfile && sudo swapon /swapfile
echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab

# 3. Fill secrets
cp docker/.env.example docker/.env
# edit docker/.env — set MYSQL_*, MQTT_USERNAME/PASSWORD, API_KEY,
# FLASK_SECRET_KEY, LOGIN_USERNAME/PASSWORD, MQTT_PUBLIC_HOST=<server IP>
ln -sf docker/.env .env   # so docker-compose ${...} interpolation finds it

# 4. Generate the mosquitto password file (must match MQTT_USERNAME/PASSWORD)
docker run --rm -v "$(pwd)/docker/mosquitto:/mosquitto" eclipse-mosquitto:2.0 \
    mosquitto_passwd -c -b /mosquitto/passwd "$MQTT_USERNAME" "$MQTT_PASSWORD"

# 5. Start
docker compose up -d
docker compose logs -f       # watch first boot — mysql init.sql runs here

# 6. Open https://<server-ip>:5000  →  log in with LOGIN_USERNAME/PASSWORD
```

### ESP32 side

```bash
cp firmware/secretsExample.h firmware/secrets.h
# fill HOST, MQTT_USERNAME, MQTT_PASSWORD, FLASK_API_KEY (=== API_KEY in .env)
cd firmware && pio run --target upload && pio device monitor --baud 115200
```

`HOST` is shared by the MQTT broker (port 1883) and the Flask `/verify` endpoint
(port 5000), so it must be the public IP / domain of the docker host.
