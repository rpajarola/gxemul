import subprocess
import socket
import time
import sys
import re
import threading

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

t = threading.Thread(target=get_port, daemon=True)
t.start()

for _ in range(50):
    if port:
        break
    time.sleep(0.1)

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(('127.0.0.1', port))

buf = ""
t0 = time.time()
while time.time() - t0 < 60:
    s.settimeout(0.5)
    try:
        data = s.recv(4096).decode('latin1', errors='replace')
        if data:
            buf += data
            clean = re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', data)
            sys.stdout.write(clean)
            sys.stdout.flush()
            if "Supplementary menu..." in buf:
                time.sleep(1)
                # Try sending '4\r' or down arrow
                # Let's see if sending down arrow key: \x1b[B
                print("\n[SENDING: down arrow]")
                s.sendall(b"\x1b[B")
                time.sleep(0.5)
                d1 = s.recv(4096).decode('latin1', errors='replace')
                print(f"[RECV AFTER DOWN: {repr(d1)}]")
                s.sendall(b"\x1b[B")
                time.sleep(0.5)
                d2 = s.recv(4096).decode('latin1', errors='replace')
                print(f"[RECV AFTER DOWN 2: {repr(d2)}]")
                s.sendall(b"\x1b[B")
                time.sleep(0.5)
                d3 = s.recv(4096).decode('latin1', errors='replace')
                print(f"[RECV AFTER DOWN 3: {repr(d3)}]")
                s.sendall(b"\r")
                time.sleep(2)
                d4 = s.recv(4096).decode('latin1', errors='replace')
                print(f"[RECV AFTER ENTER:\n{re.sub(r'\x1b\\[[0-9;]*[a-zA-Z]', '', d4)}]")
                break
    except socket.timeout:
        pass

time.sleep(2)
proc.terminate()
