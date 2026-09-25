import serial
import time
import sys

# --- CONFIGURATION ---
PORT = '/dev/cu.usbserial-110'
BAUD = 115200
OUTPUT_FILE = 'car_telemetry_dump.csv'

END_MARKER   = '===== end of dump ====='
BEGIN_PREFIX = '===== /log'   # first segment banner = start of real data
# Increased to 20 seconds to wait out delay(5000), BLE init, GPS init, and Network init
SETTLE_S     = 20             
TIMEOUT_S    = 300            # safety cap (a full ~3.3MB dump @115200 takes ~5 min)

def main():
    print(f"[+] Opening {PORT} @ {BAUD}")
    
    # 1. Initialize the port object WITHOUT opening it immediately
    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 1
    
    # 2. Force hardware flow control off BEFORE the OS can toggle pins
    # This prevents the Mac from rebooting the ESP32 or holding it in a dead state
    ser.dtr = False
    ser.rts = False
    
    try:
        ser.open() # Now open it safely
    except Exception as e:
        print(f"[-] Failed to open port: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"[+] Port opened safely. Waiting {SETTLE_S} seconds for ESP32 to finish boot sequence...")
    time.sleep(SETTLE_S)
    ser.reset_input_buffer()

    deadline = time.time() + TIMEOUT_S
    capturing = False
    header_written = False
    last_send = 0.0

    print(f"[+] Initialization wait over. Attempting to extract logs...")

    with open(OUTPUT_FILE, 'w', encoding='utf-8', newline='') as f:
        while time.time() < deadline:
            
            # Keep nudging DUMP until the dump starts 
            if not capturing and time.time() - last_send > 3:
                ser.write(b'DUMP\n')
                last_send = time.time()
                print('[+] Sent DUMP command to device...')

            raw = ser.readline()
            if not raw:
                continue
            
            line = raw.decode('utf-8', 'ignore').rstrip('\r\n')

            # Look for the absolute end of the transmission
            if line.strip() == END_MARKER:
                print('[+] Empty log' if not capturing else '[+] Done compiling logs.')
                break
            
            # Wait until we see the first actual log segment before capturing
            if not capturing:
                if line.startswith(BEGIN_PREFIX):
                    capturing = True          # Real data begins; drop the banner itself
                    print('[+] Stream started. Saving rows...')
                continue                      # Discard pre-dump debug noise
            
            # Filter out subsequent file-segment banners
            if line.startswith('====='):       
                continue
            
            # Deduplicate the CSV headers from the rolling segments
            if line.startswith('millis,utc'):  
                if header_written:
                    continue
                header_written = True
            
            # Write the clean data row to the file and print it
            f.write(line + '\n')
            print(line)
            
        else:
            print('[-] Timed out before end marker was received.', file=sys.stderr)

    ser.close()
    print(f'[+] Extraction complete. Saved to: {OUTPUT_FILE}')

if __name__ == '__main__':
    main()