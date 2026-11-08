import subprocess, socket, time, re
from vt100 import VT100

def drain(s, vt=None, pause=0.5):
    time.sleep(pause)
    out = ""
    while True:
        try:
            s.settimeout(0.3)
            d = s.recv(4096).decode('latin1', errors='replace')
            if d:
                out += d
                if vt: vt.feed(d)
            else: break
        except: break
    return out

def send_down(s, count=1):
    for _ in range(count):
        s.sendall(b"\x9bB")
        time.sleep(0.3)

p = subprocess.Popen(['./gxemul', '-e', 'jensen', '-x', '-M', '64', '-d', '0:experiments/nt/nt_disk.img', '-d', '4:/tmp/nt31_alpha.iso', 'media/rom/jensen_v1_3.rom'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
time.sleep(1)
s = socket.socket()
s.connect(('127.0.0.1', 4000))
vt = VT100()

print("Waiting for boot...", flush=True)
t0 = time.time()
buf = ""
while time.time() - t0 < 35:
    s.settimeout(0.5)
    try:
        d = s.recv(1024).decode('latin1', errors='replace')
        if d:
            vt.feed(d)
            buf += d
            if 'Use the arrow keys to select, then press Enter.' in buf:
                break
    except socket.timeout: pass

print("Booted! Navigating to Supplementary menu...", flush=True)
time.sleep(1)
drain(s, vt, 0.5)

# Option 3: Supplementary menu
send_down(s, 3)
s.sendall(b"\n")
time.sleep(2.0)
drain(s, vt, 0.5)
print("=== IN SUPPLEMENTARY MENU ===")
print(vt.render()[:500])

# Option 2: Set up the system...
print("Entering 'Set up the system...'...")
send_down(s, 2)
s.sendall(b"\n")
time.sleep(2.0)
drain(s, vt, 0.5)
print("=== IN SETUP MENU ===")
print(vt.render()[:1000])

# In Setup menu:
# Let's see the menu items from our dump earlier:
# 0: Set system time
# 1: Set default environment variables
# 2: Set default configuration
# ...
# 10: Supplementary menu, and save changes...

print("Selecting 'Set default environment variables' (item 1)...")
send_down(s, 1)
s.sendall(b"\n")
time.sleep(2.0)
drain(s, vt, 0.5)

print("Selecting 'Set default configuration' (item 2)...")
send_down(s, 1)
s.sendall(b"\n")
time.sleep(2.0)
drain(s, vt, 0.5)

print("Selecting 'Supplementary menu, and save changes...' (item 10)...")
# From item 2, down 8 times is item 10:
send_down(s, 8)
s.sendall(b"\n")
time.sleep(4.0)

out = drain(s, vt, 1.0)
print("OUTPUT AFTER SAVE:")
print(out)
print("=== RENDERED SCREEN ===")
print(vt.render())

p.kill()
stdout, _ = p.communicate()
with open('/tmp/gxemul_save_stdout.log', 'wb') as f:
    f.write(stdout)
