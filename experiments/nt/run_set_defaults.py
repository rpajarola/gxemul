import subprocess, socket, time, sys
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

log_file = open('/tmp/gxemul_setdefs.log', 'w')
p = subprocess.Popen(['./gxemul', '-e', 'jensen', '-x', '-M', '64', '-d', '0:experiments/nt/nt_disk.img', '-d', '4:/tmp/nt31_alpha.iso', 'media/rom/jensen_v1_3.rom'], stdout=log_file, stderr=subprocess.STDOUT)

time.sleep(1)
s = socket.socket()
s.connect(('127.0.0.1', 4000))
vt = VT100()

print("Waiting for boot menu...", flush=True)
wait_for_boot(s, vt)
print("=== INITIAL BOOT MENU ===", flush=True)
print(vt.render(), flush=True)

# Supplementary menu is option 3
print("\n>>> Entering Supplementary menu... <<<", flush=True)
send_down(s, 3)
s.sendall(b"\r")
time.sleep(2.0)
drain(s, vt, 0.5)

print("=== SUPPLEMENTARY MENU ===", flush=True)
print(vt.render(), flush=True)

# In Supplementary menu:
# 0: Install new firmware
# 1: Install Windows NT from CD-ROM
# 2: Set up the system...
# 3: List available boot devices
# 4: Boot menu...
print("\n>>> Selecting 'Set up the system...' <<<", flush=True)
send_down(s, 2)
s.sendall(b"\r")
time.sleep(2.0)
drain(s, vt, 0.5)

print("=== SETUP MENU ===", flush=True)
print(vt.render(), flush=True)

# In Setup menu:
# Item 0: Set system time
# Item 1: Set default environment variables
# Item 2: Set default configuration
# ...
# Item 10: Supplementary menu, and save changes...

print("\n>>> Selecting 'Set default environment variables' (item 1) <<<", flush=True)
send_down(s, 1)
s.sendall(b"\r")
time.sleep(2.0)
drain(s, vt, 0.5)

print("\n>>> Selecting 'Set default configuration' (item 2) <<<", flush=True)
send_down(s, 1)
s.sendall(b"\r")
time.sleep(2.0)
drain(s, vt, 0.5)

print("\n>>> Selecting 'Supplementary menu, and save changes...' (item 10) <<<", flush=True)
send_down(s, 8)
s.sendall(b"\r")
time.sleep(5.0)
drain(s, vt, 0.5)

print("=== SCREEN AFTER SAVE ===", flush=True)
print(vt.render(), flush=True)

p.terminate()
p.wait()
