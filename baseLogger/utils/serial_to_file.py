# Save serial output and UTC lines to separate log files.

import serial
from datetime import datetime
import os

# Adjust the serial port name and baud rate according to your setup
serial_port = '/dev/ttyUSB0'
baud_rate = 115200


current_time = datetime.now().strftime("%m%d%H%M")
log_directory = f'../logs/{current_time}'
os.makedirs(log_directory, exist_ok=True)
log_file_name = os.path.join(log_directory, "log.txt")
utc_file_name = os.path.join(log_directory, "utc.txt")

ser = serial.Serial(serial_port, baud_rate)

with open(log_file_name, 'w') as f_log, open(utc_file_name, 'w') as f_utc:
    while True:
        if ser.in_waiting > 0:
            data = ser.readline().decode().strip()
            print(data)
            if data.startswith("UTC: "):
              f_utc.write(data + '\n')
            else: 
              f_log.write(data + '\n')


ser.close()
