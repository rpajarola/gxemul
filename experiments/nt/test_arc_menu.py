import subprocess
import socket
import time
import sys
import re

print("Starting GXemul with Jensen in ARC mode...")
proc = subprocess.Popen([
    "./gxemul", "-e", "jensen", "-x", "-M", "64",
    "-d", "0:experiments/nt/nt_disk.img",
    "-d", "4:/tmp/nt31_alpha.iso",
    "media/rom/jensen_v1_3.rom"
], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

port = None
def get_port():
    global port
    for line in iter(proc.stdout.readline, b''):
        l = line.decode('latin1', errors='replace')
        m = re.search(r'telnet localhost (\d+): tty0', l)
        if m:
            port = int(m.group(1))
            break

import threading
t = threading.Thread(target=get_port, daemon=True)
t.start()

for _ in range(50):
    if port:
        break
    time.sleep(0.1)

print(f"Connecting to port {port}...")
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(('127.0.0.1', port))
print("Connected!")

buf = ""
t0 = time.time()
while time.time() - t0 < 60:
    s.settimeout(0.5)
    try:
        data = s.recv(4096).decode('latin1', errors='replace')
        if data:
            buf += data
            # Strip some ANSI escape codes for clean terminal output
            clean = re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', data)
            sys.stdout.write(clean)
            sys.stdout.flush()
            if "Supplementary menu..." in buf:
                print("\n[REACHED BOOT MENU! Navigating to Supplementary menu...]")
                time.sleep(1)
                # Send Down arrow 3 times, then Enter
                s.sendall(b"\x1b[B\x1b[B\x1b[B\r\n")
                buf = ""
                time.sleep(2)
                d = s.recv(4096).decode('latin1', errors='replace')
                print("\n[SUPPLEMENTARY MENU:]\n", re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', d))
                break
    except socket.timeout:
        pass

time.sleep(2)
proc.terminate()
