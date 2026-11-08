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
while True:
    try:
        s.settimeout(0.2)
        s.recv(4096)
    except:
        break

# Navigate to Supplementary menu -> Set up the system
s.sendall(b"\x9bB\x9bB\x9bB\r")
time.sleep(1.0)
s.sendall(b"\x9bB\x9bB\r")
time.sleep(1.0)

# In Setup menu:
# Item 1: Set default environment variables
print(">>> Selecting 'Set default environment variables' <<<")
s.sendall(b"\x9bB\r")
time.sleep(2.0)

resp = ""
while True:
    try:
        s.settimeout(0.5)
        c = s.recv(4096).decode('latin1', errors='replace')
        if c: resp += c
        else: break
    except: break

clean = re.sub(r'[\x1b\x9b]\[?[0-9;]*[a-zA-Z]', '', resp)
print("\n=== SCREEN AFTER 'Set default environment variables' ===")
print(clean)
print("=====================================================")

p.terminate()
