#Read data form serial and stor in logfile

import serial
from datetime import datetime
import os

# Adjust the serial port name and baud rate according to your setup
serial_port = '/dev/ttyUSB0'  # Replace with your serial port name
baud_rate = 115200


# Generate current timestamp (yymmddhhmm)
current_time = datetime.now().strftime("%m%d%H%M")
# Create the 'logs' directory if it doesn't exist
log_directory = f'../logs/{current_time}'
os.makedirs(log_directory, exist_ok=True)
log_file_name = os.path.join(log_directory, "log.txt")
utc_file_name = os.path.join(log_directory, "utc.txt")

# Open serial port
ser = serial.Serial(serial_port, baud_rate)

# Open a file to write data received from serial
with open(log_file_name, 'w') as f_log, open(utc_file_name, 'w') as f_utc:
    while True:
        if ser.in_waiting > 0:
            data = ser.readline().decode().strip()  # Read a line from serial and decode from bytes
            print(data)  # Optional: Print the received data to console
            if data.startswith("UTC: "):
              f_utc.write(data + '\n')  # Write data to file
            else: 
              f_log.write(data + '\n')  # Write data to file


# Close the serial port when done
ser.close()
