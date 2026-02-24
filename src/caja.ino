#include "SparkFun_AS7265X.h"
#include <Wire.h>
#include <Arduino.h>
#include <LittleFS.h>

struct sensorconfig {
  String lightMode;
  int clockspeed = 400000;
  int sampleN;                  // Sensor configuration parameters
  String gain;            
  String MeasurementeMode;
  int integrationCycles;
  bool state;
};

AS7265X sensor;

const char* path = "/lecturas.csv";
sensorconfig config;

struct payload {
  float datos[18];
  int integration;
  char Mode[12];
  char gain[17];
  char date_hour[20];
  char sample_id[16];
};

struct directions {
  String sensorconfig[6];
  String bypass;
};

void readDirections() {
  // TODO: Implement reading directions from file
}



void setup() {
  Serial.begin(115200);
  readDirections();
}

void loop() {
  for (int i = 0; i < config.sampleN; i++) {
    eneableSense(config.clockspeed);
    String *readings = calibration();
    payload p;
    for(int j = 0; j < 18; j++) {
      p.datos[j] = readings[j].toFloat();
    }
    saveMeasurement(p);
    delete[] readings;
  }
}

String *calibration() {  //obtener mediciones
  String *data = new String[18];

  data[0] = String(sensor.getCalibratedA());
  data[1] = String(sensor.getCalibratedB());
  data[2] = String(sensor.getCalibratedC());
  data[3] = String(sensor.getCalibratedD());
  data[4] = String(sensor.getCalibratedE());
  data[5] = String(sensor.getCalibratedF());
  data[6] = String(sensor.getCalibratedG());
  data[7] = String(sensor.getCalibratedH());
  data[8] = String(sensor.getCalibratedR());
  data[9] = String(sensor.getCalibratedI());
  data[10] = String(sensor.getCalibratedS());
  data[11] = String(sensor.getCalibratedJ());
  data[12] = String(sensor.getCalibratedT());
  data[13] = String(sensor.getCalibratedU());
  data[14] = String(sensor.getCalibratedV());
  data[15] = String(sensor.getCalibratedW());
  data[16] = String(sensor.getCalibratedK());
  data[17] = String(sensor.getCalibratedL());
  return data;
}

void eneableSense(int clockspeed) {
  while (!sensor.begin()) {
  }
  config.state = sensor.begin();
  if (!config.state) {Wire.setClock(clockspeed);}
}

void saveMeasurement(payload d){
  File file = LittleFS.open(path, "a");
  if(!file) return;

  char buffer[512];
  int pos = 0;

  // 1 Escribe 18 canales
  for (int i = 0; i < 18; i++){
    pos += sprintf(&buffer[pos], "%.2f,", d.datos[i]);
  }

  // 2 Escribir strings
  pos += sprintf(&buffer[pos], "%d,%s,%s,%s,%s\n", d.integration, d.Mode, d.gain, d.date_hour, d.sample_id);

  file.write((const uint8_t*)buffer, pos);
  file.close();
}

