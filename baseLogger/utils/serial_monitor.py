# Print serial communication to console without storing

import serial
from datetime import datetime
import os

# Adjust the serial port name and baud rate according to your setup
serial_port = '/dev/ttyUSB0'  # Replace with your serial port name
baud_rate = 115200

# Open serial port
ser = serial.Serial(serial_port, baud_rate)

# Open a file to write data received from serial
while True:
    if ser.in_waiting > 0:
        data = ser.readline().decode().strip()  # Read a line from serial and decode from bytes
        print(data)  # Print the received data to console


# Close the serial port when done
ser.close()
