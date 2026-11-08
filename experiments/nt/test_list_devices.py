import subprocess, socket, time
from vt100 import VT100

def wait_for_boot(s, vt):
    t0 = time.time()
    buf = ""
    while time.time() - t0 < 45:
        s.settimeout(0.5)
        try:
            d = s.recv(1024).decode('latin1', errors='replace')
            if d:
                vt.feed(d)
                buf += d
                if 'Use the arrow keys to select, then press Enter.' in buf:
                    break
        except socket.timeout:
            pass
    time.sleep(2.0)
    drain(s, vt, 0.5)

def drain(s, vt, pause=0.5):
    time.sleep(pause)
    while True:
        try:
            s.settimeout(0.2)
            d = s.recv(4096).decode('latin1', errors='replace')
            if d: vt.feed(d)
            else: break
        except: break

def send_down(s, count=1):
    for _ in range(count):
        s.sendall(b"\x9bB")
        time.sleep(0.3)

log_file = open('/tmp/gxemul_devlist.log', 'w')
p = subprocess.Popen(['./gxemul', '-e', 'jensen', '-x', '-M', '64', '-d', '0:experiments/nt/nt_disk.img', '-d', '4:/tmp/nt31_alpha.iso', 'media/rom/jensen_v1_3.rom'], stdout=log_file, stderr=subprocess.STDOUT)

time.sleep(1)
s = socket.socket()
s.connect(('127.0.0.1', 4000))
vt = VT100()

print("Waiting for boot menu...")
wait_for_boot(s, vt)
print("=== INITIAL BOOT MENU ===")
print(vt.render())

# Go to Supplementary menu (3 down arrows + Enter)
print("\n>>> Entering Supplementary menu... <<<")
send_down(s, 3)
s.sendall(b"\n")

# Wait for Supplementary menu prompt
t0 = time.time()
buf = ""
while time.time() - t0 < 10:
    s.settimeout(0.5)
    try:
        d = s.recv(1024).decode('latin1', errors='replace')
        if d:
            vt.feed(d)
            buf += d
            if 'Supplementary menu:' in buf: break
    except socket.timeout: pass

time.sleep(2.0)
drain(s, vt, 0.5)
print("=== SUPPLEMENTARY MENU ===")
print(vt.render())

# In Supplementary menu:
# 0: Install new firmware
# 1: Install Windows NT from CD-ROM
# 2: Set up the system...
# 3: List available boot devices
print("\n>>> Selecting 'List available boot devices' <<<")
send_down(s, 3)
s.sendall(b"\n")

t0 = time.time()
while time.time() - t0 < 5.0:
    s.settimeout(0.5)
    try:
        d = s.recv(1024).decode('latin1', errors='replace')
        if d: vt.feed(d)
    except socket.timeout: pass

drain(s, vt, 0.5)
print("\n=== SCREEN AFTER 'List available boot devices' ===")
print(vt.render())
print("==================================================")

p.terminate()
