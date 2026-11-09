import subprocess, socket, time, sys, re, os
from vt100 import VT100

def wait_for_text(s, vt, target, timeout=30.0):
    t0 = time.time()
    buf = ""
    while time.time() - t0 < timeout:
        s.settimeout(0.5)
        try:
            d = s.recv(1024).decode('latin1', errors='replace')
            if d:
                vt.feed(d)
                buf += d
                if target in buf:
                    return True
        except socket.timeout:
            pass
    return False

def drain(s, vt, pause=0.5):
    time.sleep(pause)
    while True:
        try:
            s.settimeout(0.2)
            d = s.recv(4096).decode('latin1', errors='replace')
            if d:
                vt.feed(d)
            else:
                break
        except:
            break

log_file = open('/tmp/gxemul_nt.log', 'w')
p = subprocess.Popen(['./gxemul', '-e', 'jensen', '-x', '-M', '64', '-d', '0:experiments/nt/nt_disk.img', '-d', '4:/tmp/nt31_alpha.iso', 'media/rom/jensen_v1_3.rom'], stdout=log_file, stderr=subprocess.STDOUT)

port = None
t0 = time.time()
while time.time() - t0 < 5:
    time.sleep(0.1)
    if os.path.exists('/tmp/gxemul_nt.log'):
        with open('/tmp/gxemul_nt.log', 'r') as f:
            m = re.search(r'telnet localhost (\d+): tty0', f.read())
            if m:
                port = int(m.group(1))
                break

if not port:
    p.kill()
    sys.exit("Failed to find tty0 port")

print(f"Connecting to port {port}...")
s = socket.socket()
s.connect(('127.0.0.1', port))
vt = VT100()

# Abort mem test
for _ in range(8):
    s.sendall(b" ")
    time.sleep(0.15)

print("Waiting for boot menu...")
if not wait_for_text(s, vt, 'Use the arrow keys to select, then press Enter.', timeout=30.0):
    print("Timeout waiting for boot menu!")
    p.terminate()
    sys.exit(1)

drain(s, vt, 1.0)
print("=== BOOT MENU ===")
print(vt.render())

print("\n>>> Moving to Supplementary menu... <<<")
time.sleep(0.5)
s.sendall(b"\x9bB")
time.sleep(0.2)
s.sendall(b"\x9bB")
time.sleep(0.2)
s.sendall(b"\x9bB")
time.sleep(0.2)
s.sendall(b"\r")

if not wait_for_text(s, vt, 'Supplementary menu:', timeout=10.0):
    print("Timeout waiting for Supplementary menu text!")

if not wait_for_text(s, vt, 'Use the arrow keys to select, then press Enter.', timeout=10.0):
    print("Timeout waiting for prompt!")

drain(s, vt, 0.5)
print("=== SUPPLEMENTARY MENU ===")
print(vt.render())

print("\n>>> Selecting 'Install Windows NT from CD-ROM' <<<")
s.sendall(b"\x9bB")
time.sleep(0.5)
s.sendall(b"\r")

t0 = time.time()
while time.time() - t0 < 15.0:
    try:
        s.settimeout(0.5)
        d = s.recv(4096).decode('latin1', errors='replace')
        if d:
            vt.feed(d)
    except:
        pass

drain(s, vt, 1.0)
print("=== SCREEN AFTER SELECTING INSTALL NT ===")
print(vt.render())
print("=========================================")

p.terminate()
