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
            if "Use the arrow keys to select, then press Enter" in buf:
                time.sleep(1.5)
                print("\n\n>>> BOOT MENU IS READY! Testing Down Arrow <<<\n")
                # Send Down arrow
                s.sendall(b"\x1b[B")
                time.sleep(1.0)
                d1 = s.recv(4096).decode('latin1', errors='replace')
                print("[RECV 1:]\n", repr(d1))
                matches = re.findall(r'(\d+);5H.*?7m([^\x1b\x9b]+)', d1)
                print(f"--> Highlight matches: {matches}")

                print("\n>>> Testing Down Arrow 2 <<<\n")
                s.sendall(b"\x1b[B")
                time.sleep(1.0)
                d2 = s.recv(4096).decode('latin1', errors='replace')
                print("[RECV 2:]\n", repr(d2))
                matches2 = re.findall(r'(\d+);5H.*?7m([^\x1b\x9b]+)', d2)
                print(f"--> Highlight matches: {matches2}")

                print("\n>>> Testing Down Arrow 3 <<<\n")
                s.sendall(b"\x1b[B")
                time.sleep(1.0)
                d3 = s.recv(4096).decode('latin1', errors='replace')
                print("[RECV 3:]\n", repr(d3))
                matches3 = re.findall(r'(\d+);5H.*?7m([^\x1b\x9b]+)', d3)
                print(f"--> Highlight matches: {matches3}")

                print("\n>>> Testing Enter on Supplementary Menu <<<\n")
                s.sendall(b"\r\n")
                time.sleep(2.0)
                d4 = s.recv(4096).decode('latin1', errors='replace')
                print("[RECV 4 (AFTER ENTER):]\n", re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', d4))
                break
    except socket.timeout:
        pass

time.sleep(2)
proc.terminate()
