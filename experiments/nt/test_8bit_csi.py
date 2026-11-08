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
            if "Supplementary menu..." in buf and "Use the arrow keys" in buf:
                time.sleep(2.0)
                print("\n\n>>> SENDING 8-BIT CSI DOWN: \\x9bB <<<\n")
                s.sendall(b"\x9bB")
                time.sleep(1.0)
                d1 = s.recv(4096).decode('latin1', errors='replace')
                print(f"[RECV 1: {repr(d1)}]")
                matches = re.findall(r'(\d+);5H.*?7m([^\x1b\x9b]+)', d1)
                print(f"--> Highlight matches: {matches}")
                break
    except socket.timeout:
        pass

time.sleep(2)
proc.terminate()
