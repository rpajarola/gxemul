import subprocess, socket, time, re

log_file = open('/tmp/gxemul_direct.log', 'w')
p = subprocess.Popen(['./gxemul', '-e', 'jensen', '-x', '-M', '64', '-d', '0:experiments/nt/nt_disk.img', '-d', '4:/tmp/nt31_alpha.iso', 'media/rom/jensen_v1_3.rom'], stdout=log_file, stderr=subprocess.STDOUT)

time.sleep(1)
s = socket.socket()
s.connect(('127.0.0.1', 4000))
s.setblocking(False)

full_log = []

def read_all():
    res = b""
    while True:
        try:
            d = s.recv(4096)
            if not d: break
            res += d
        except BlockingIOError:
            break
        except:
            break
    if res:
        full_log.append(res)
    return res

print("Waiting for boot...", flush=True)
t0 = time.time()
while time.time() - t0 < 35:
    time.sleep(0.5)
    data = read_all()
    if b'Use the arrow keys to select, then press Enter.' in data:
        print("Boot menu ready!", flush=True)
        break

time.sleep(1.0)
read_all()

def key(k, desc=""):
    print(f"Sending {desc}...", flush=True)
    s.sendall(k)
    time.sleep(0.6)
    read_all()

def clean_str(b):
    text = b.decode('latin1', errors='replace')
    return re.sub(r'[\x1b\x9b]\[?[0-9;]*[a-zA-Z]', '', text)

# Boot menu: Supplementary menu is down 3 times
key(b"\x9bB", "down 1")
key(b"\x9bB", "down 2")
key(b"\x9bB", "down 3")
key(b"\r", "enter (Supplementary menu)")
time.sleep(1.5)
scr = read_all()
print("=== AFTER ENTER SUPPLEMENTARY ===")
print(clean_str(scr))

# Supplementary menu:
# 0: Install new firmware
# 1: Install Windows NT from CD-ROM
# 2: Set up the system...
key(b"\x9bB", "down 1")
key(b"\x9bB", "down 2")
key(b"\r", "enter (Set up the system)")
time.sleep(2.0)
scr = read_all()
print("=== AFTER ENTER SETUP MENU ===")
print(clean_str(scr))

# Setup menu:
# 0: Set system time
# 1: Set default environment variables
# 2: Set default configuration
# ...
key(b"\x9bB", "down 1 (Set default environment variables)")
key(b"\r", "enter (Set default env vars)")
time.sleep(2.0)
scr = read_all()
print("=== AFTER DEFAULT ENV VARS ===")
print(clean_str(scr))

key(b"\x9bB", "down 1 (Set default configuration)")
key(b"\r", "enter (Set default config)")
time.sleep(2.0)
scr = read_all()
print("=== AFTER DEFAULT CONFIG ===")
print(clean_str(scr))

# Now let's see how many items to save changes: item 11 is 'Supplementary menu, and save changes...'
# From item 2, we need to go down 9 times (2 + 9 = 11)
for i in range(9):
    key(b"\x9bB", f"down {i+1}/9 to save changes")
    time.sleep(0.2)
key(b"\r", "enter (Save changes)")
time.sleep(6.0)
scr = read_all()
print("=== AFTER SAVE CHANGES ===")
print(clean_str(scr))

p.terminate()
p.wait()

with open('/tmp/all_screens.log', 'w') as f:
    for chunk in full_log:
        f.write(clean_str(chunk) + "\n---CHUNK---\n")
