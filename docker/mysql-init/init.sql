CREATE DATABASE IF NOT EXISTS espectrografo;
USE espectrografo;

CREATE TABLE IF NOT EXISTS experimentos (
    id INT AUTO_INCREMENT PRIMARY KEY,
    exp_id VARCHAR(32) NOT NULL UNIQUE,
    timestamp_ms BIGINT,
    num_measurements INT,
    gain INT, mode INT, int_cycles INT,
    led_white_ma INT, led_ir_ma INT, led_uv_ma INT,
    cal_valid BOOLEAN,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS mediciones (
    id INT AUTO_INCREMENT PRIMARY KEY,
    exp_id VARCHAR(32) NOT NULL,
    meas_index INT,
    ch1 FLOAT, ch2 FLOAT, ch3 FLOAT, ch4 FLOAT, ch5 FLOAT,
    ch6 FLOAT, ch7 FLOAT, ch8 FLOAT, ch9 FLOAT, ch10 FLOAT,
    ch11 FLOAT, ch12 FLOAT, ch13 FLOAT, ch14 FLOAT, ch15 FLOAT,
    ch16 FLOAT, ch17 FLOAT, ch18 FLOAT,
    FOREIGN KEY (exp_id) REFERENCES experimentos(exp_id)
);

CREATE TABLE IF NOT EXISTS calibraciones (
    id INT AUTO_INCREMENT PRIMARY KEY,
    exp_id VARCHAR(32) NOT NULL,
    ch1 FLOAT, ch2 FLOAT, ch3 FLOAT, ch4 FLOAT, ch5 FLOAT,
    ch6 FLOAT, ch7 FLOAT, ch8 FLOAT, ch9 FLOAT, ch10 FLOAT,
    ch11 FLOAT, ch12 FLOAT, ch13 FLOAT, ch14 FLOAT, ch15 FLOAT,
    ch16 FLOAT, ch17 FLOAT, ch18 FLOAT,
    FOREIGN KEY (exp_id) REFERENCES experimentos(exp_id)
);
