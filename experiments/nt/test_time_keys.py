import subprocess, socket, time, re, sys, os

env = os.environ.copy()
env['DEBUG_SERIAL'] = '1'

p = subprocess.Popen(['./gxemul', '-e', 'jensen', '-x', '-M', '64', '-d', '0:experiments/nt/nt_disk.img', '-d', '4:/tmp/nt31_alpha.iso', 'media/rom/jensen_v1_3.rom'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)

time.sleep(1)
s = socket.socket()
s.connect(('127.0.0.1', 4000))

buf = ""
t0 = time.time()
while time.time() - t0 < 30:
    s.settimeout(0.5)
    try:
        d = s.recv(1024).decode('latin1', errors='replace')
        if d:
            buf += d
            if 'Use the arrow keys to select, then press Enter.' in buf:
                break
    except socket.timeout:
        pass

time.sleep(1.5)
while True:
    try:
        s.settimeout(0.2)
        s.recv(4096)
    except:
        break

# Navigate to Supplementary -> Set up system
s.sendall(b"\x9bB\x9bB\x9bB\n")
time.sleep(1.0)
s.sendall(b"\x9bB\x9bB\n")
time.sleep(1.0)

# Enter Set system time
s.sendall(b"\n")
time.sleep(1.5)

# Drain
while True:
    try:
        s.settimeout(0.2)
        s.recv(4096)
    except:
        break

keys_to_try = [
    ("Enter CR", b"\r"),
    ("Enter LF", b"\n"),
    ("Esc", b"\x1b"),
    ("8bit Left", b"\x9bD"),
    ("8bit Right", b"\x9bC"),
    ("8bit Up", b"\x9bA"),
    ("8bit Down", b"\x9bB"),
    ("Space", b" "),
    ("Char q", b"q"),
    ("Char x", b"x"),
    ("Ctrl-C", b"\x03"),
]

for name, k in keys_to_try:
    print(f"\n--- Testing in Time screen: {name} ({repr(k)}) ---")
    s.sendall(k)
    time.sleep(0.5)
    resp = ""
    while True:
        try:
            s.settimeout(0.2)
            c = s.recv(4096).decode('latin1', errors='replace')
            if c: resp += c
            else: break
        except: break
    clean = re.sub(r'[\x1b\x9b]\[?[0-9;]*[a-zA-Z]', '', resp)
    print("OUTPUT:", repr(resp[:60]))
    if "Setup" in resp or "menu" in resp or "Setup" in clean:
        print("--> EXIT TO SETUP MENU DETECTED!")
        break

p.terminate()
