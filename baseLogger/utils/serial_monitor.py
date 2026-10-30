# Print serial communication to console without storing

import serial
from datetime import datetime
import os

# Adjust the serial port name and baud rate according to your setup
serial_port = '/dev/ttyUSB0'
baud_rate = 115200

ser = serial.Serial(serial_port, baud_rate)

while True:
    if ser.in_waiting > 0:
        data = ser.readline().decode().strip()
        print(data)


ser.close()
