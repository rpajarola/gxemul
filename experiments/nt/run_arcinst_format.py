import sys, subprocess, socket, time
sys.path.append("experiments/nt")
from vt100 import VT100

def drain(s, vt, pause=0.5):
    time.sleep(pause)
    out = ""
    while True:
        try:
            s.settimeout(0.2)
            d = s.recv(4096).decode("latin1", errors="replace")
            if d:
                vt.feed(d)
                out += d
            else:
                break
        except:
            break
    return out

def send_keys(s, keys, delay=0.05):
    for ch in keys:
        if isinstance(ch, int):
            s.sendall(bytes([ch]))
        elif isinstance(ch, str):
            s.sendall(ch.encode("latin1"))
        elif isinstance(ch, bytes):
            s.sendall(ch)
        time.sleep(delay)

log_file = open("/tmp/gxemul_format_syspart.log", "w")
p = subprocess.Popen([
    "./gxemul", "-e", "jensen", "-x", "-M", "64",
    "-d", "0:experiments/nt/nt_disk.img",
    "-d", "4:/tmp/nt31_alpha.iso",
    "media/rom/jensen_v1_3.rom"
], stdout=log_file, stderr=subprocess.STDOUT)

time.sleep(1)
s = socket.socket()
s.connect(("127.0.0.1", 4000))
vt = VT100()

# Abort memory test by sending spaces
for _ in range(6):
    s.sendall(b" ")
    time.sleep(0.5)

# Wait for boot menu
print("Waiting for boot menu...", flush=True)
t0 = time.time()
while time.time() - t0 < 25:
    s.settimeout(0.5)
    try:
        d = s.recv(1024).decode("latin1", errors="replace")
        if d:
            vt.feed(d)
            if "Use the arrow keys to select, then press Enter." in vt.render():
                break
    except socket.timeout:
        pass

time.sleep(1.0)
drain(s, vt, 0.5)

print("\n>>> Navigating to 'Run a program' <<<", flush=True)
s.sendall(b"\x9bB")
time.sleep(0.3)
s.sendall(b"\x9bB")
time.sleep(0.3)
s.sendall(b"\n")
time.sleep(2.0)
drain(s, vt, 0.5)

cmd = "scsi(0)cdrom(4)fdisk(0)\\alpha\\arcinst.exe\n"
print(f"\n>>> Running {cmd.strip()} <<<", flush=True)
send_keys(s, cmd)

# Wait for Arc Installation Program
t0 = time.time()
while time.time() - t0 < 10.0:
    s.settimeout(0.5)
    try:
        d = s.recv(1024).decode('latin1', errors='replace')
        if d:
            vt.feed(d)
            if 'Configure Partitions' in d or 'Configure Partitions' in vt.render():
                break
    except socket.timeout:
        pass

drain(s, vt, 0.5)

print("\n>>> Selecting 'Configure Partitions' <<<", flush=True)
s.sendall(b"\n")
time.sleep(1.5)
drain(s, vt, 0.5)

print("\n>>> Selecting 'Create System Partition' <<<", flush=True)
s.sendall(b"\n")
time.sleep(2.0)
drain(s, vt, 0.5)

# Send partition size 10 MB
print("\n>>> Sending 10 MB <<<", flush=True)
send_keys(s, "10\n")
time.sleep(2.0)
drain(s, vt, 0.5)

# Press any key to continue
print("\n>>> Pressing key to continue... <<<", flush=True)
s.sendall(b" ")
time.sleep(1.0)
drain(s, vt, 0.5)

print("\n=== SCREEN: MAKE THIS SYSTEM PARTITION? ===", flush=True)
print(vt.render(), flush=True)

# Answer 'y' to make this the system partition
print("\n>>> Sending 'y' <<<", flush=True)
s.sendall(b"y\n")
time.sleep(3.0)
drain(s, vt, 1.0)

print("\n=== SCREEN AFTER 'y' ===", flush=True)
print(vt.render(), flush=True)

# Wait up to 10s for formatting to complete
t0 = time.time()
while time.time() - t0 < 10.0:
    s.settimeout(0.5)
    try:
        d = s.recv(1024).decode('latin1', errors='replace')
        if d:
            vt.feed(d)
            if "complete" in vt.render().lower() or "exit" in vt.render().lower():
                break
    except socket.timeout:
        pass

drain(s, vt, 1.0)
print("\n=== FINAL FORMAT SCREEN ===", flush=True)
print(vt.render(), flush=True)

p.terminate()
p.wait()
print("\nDone!", flush=True)
