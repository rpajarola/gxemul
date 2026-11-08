import subprocess, socket, time, re, sys
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
            else:
                break
        except socket.timeout:
            pass
    time.sleep(2.0)
    drain(s, vt)

def drain(s, vt=None):
    time.sleep(0.5)
    out = ""
    while True:
        try:
            s.settimeout(0.3)
            d = s.recv(4096).decode('latin1', errors='replace')
            if d:
                out += d
                if vt: vt.feed(d)
            else:
                break
        except socket.timeout:
            break
        except:
            break
    return out

def send_down(s, count=1):
    for _ in range(count):
        s.sendall(b"\x9bB")
        time.sleep(0.4)

def send_enter(s):
    s.sendall(b"\r")
    time.sleep(0.5)

log_file = open('/tmp/gxemul_defaults.log', 'w')
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
send_enter(s)
time.sleep(1.5)
drain(s, vt)

print("=== SUPPLEMENTARY MENU ===", flush=True)
print(vt.render(), flush=True)

# In Supplementary menu:
# 0: Install new firmware
# 1: Install Windows NT from CD-ROM
# 2: Set up the system...
print("\n>>> Selecting 'Set up the system...' <<<", flush=True)
send_down(s, 2)
send_enter(s)
time.sleep(2.0)
drain(s, vt)

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
send_enter(s)
time.sleep(2.0)
out1 = drain(s, vt)
print(f"Result 1: {out1.strip()[:100]}", flush=True)

print("\n>>> Selecting 'Set default configuration' (item 2) <<<", flush=True)
send_down(s, 1)
send_enter(s)
time.sleep(2.0)
out2 = drain(s, vt)
print(f"Result 2: {out2.strip()[:100]}", flush=True)

print("\n>>> Selecting 'Supplementary menu, and save changes...' (item 10) <<<", flush=True)
send_down(s, 8)
send_enter(s)
print("Waiting 10 seconds for save to complete...", flush=True)
time.sleep(10.0)
drain(s, vt)

print("=== SCREEN AFTER SAVE ===", flush=True)
print(vt.render(), flush=True)

p.terminate()
p.wait()
print("Done!", flush=True)
