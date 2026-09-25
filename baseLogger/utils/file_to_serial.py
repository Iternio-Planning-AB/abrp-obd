# Send data from file to Arduino

import serial
import time

# Adjust the serial port name and baud rate according to your setup
serial_port = '/dev/ttyUSB0'  # Replace with your serial port name
baud_rate = 115200
file_to_send = 'data_to_send.txt'  # Replace with your file path

# Function to read data from a file and send to Arduino over serial
def send_data_from_file(file_path, serial_port, baud_rate):
    ser = serial.Serial(serial_port, baud_rate)
    with open(file_path, 'r') as file:
        for line in file:
            ser.write(line.encode())
            print(f"Sent: {line.strip()}")
            time.sleep(0.1)  # Optional: Add a delay if necessary
    ser.close()

send_data_from_file(file_to_send, serial_port, baud_rate)
