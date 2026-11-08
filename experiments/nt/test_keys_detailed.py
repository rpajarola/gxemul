import subprocess, socket, time, re, sys

p = subprocess.Popen(['./gxemul', '-e', 'jensen', '-x', '-M', '64', '-d', '0:experiments/nt/nt_disk.img', '-d', '4:/tmp/nt31_alpha.iso', 'media/rom/jensen_v1_3.rom'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

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
# Drain
s.settimeout(0.2)
while True:
    try:
        s.recv(4096)
    except:
        break

candidates = [
    ("ANSI Down", b"\x1b[B"),
    ("App Down", b"\x1bOB"),
    ("8bit Down", b"\x9bB"),
    ("VT52 Down", b"\x1bB"),
    ("Ctrl-N", b"\x0e"),
    ("Ctrl-J", b"\x0a"),
    ("Tab", b"\t"),
    ("Space", b" "),
    ("Char 2", b"2"),
    ("Char 4", b"4"),
    ("Char j", b"j"),
    ("Char J", b"J"),
    ("Char +", b"+"),
    ("Char -", b"-"),
    ("Char .", b"."),
]

for name, key in candidates:
    print(f"\n=== Testing {name}: {repr(key)} ===")
    s.sendall(key)
    time.sleep(0.8)
    resp = ""
    while True:
        try:
            chunk = s.recv(4096).decode('latin1', errors='replace')
            if chunk:
                resp += chunk
            else:
                break
        except socket.timeout:
            break
    clean = re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', resp)
    clean = re.sub(r'\x9b[0-9;]*[a-zA-Z]', '', clean)
    print(f"RAW len={len(resp)}: {repr(resp[:100])}")
    # Check if highlight sequence (\x9b7m or \x1b[7m) moved
    hl = re.findall(r'(\d+);5H.*?(?:7m|27m)([^\x1b\x9b\r\n]+)', resp)
    if hl:
        print(f"--> HIGHLIGHT DETECTED: {hl}")
    if clean.strip():
        print(f"CLEAN: {clean.strip()[:80]}")

p.terminate()
