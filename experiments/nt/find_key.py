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
            if "Supplementary menu..." in buf:
                time.sleep(1)
                break
    except socket.timeout:
        pass

# Drain
s.settimeout(0.5)
while True:
    try:
        s.recv(4096)
    except:
        break

keys_to_test = [
    ("ANSI Down", b"\x1b[B"),
    ("App Down", b"\x1bOB"),
    ("Tab", b"\t"),
    ("Space", b" "),
    ("Char j", b"j"),
    ("Char 2", b"2"),
    ("Char 4", b"4"),
    ("Ctrl-N", b"\x0e"),
]

for name, k in keys_to_test:
    print(f"\n--- Testing {name} ({repr(k)}) ---")
    s.sendall(k)
    time.sleep(0.5)
    try:
        resp = s.recv(4096).decode('latin1', errors='replace')
        print("RECV:", repr(resp))
        # Look for reverse video highlight \x9b7m or \x1b[7m
        matches = re.findall(r'(\d+);5H.*?7m([^\x1b\x9b]+)', resp)
        if matches:
            print(f"--> Highlight moved to: {matches}")
    except socket.timeout:
        print("No response (timeout)")

proc.terminate()
