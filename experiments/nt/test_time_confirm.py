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
while True:
    try:
        s.settimeout(0.2)
        s.recv(4096)
    except:
        break

print(">>> Entering Supplementary menu... <<<")
s.sendall(b"\x9bB\x9bB\x9bB\r")
time.sleep(1.0)

print(">>> Entering 'Set up the system...' <<<")
s.sendall(b"\x9bB\x9bB\r")
time.sleep(1.0)

print(">>> Selecting 'Set system time' <<<")
s.sendall(b"\r")
time.sleep(1.0)

# Drain the time ticker
while True:
    try:
        s.settimeout(0.2)
        s.recv(4096)
    except:
        break

print(">>> Pressing Enter to confirm time... <<<")
s.sendall(b"\r")
time.sleep(1.5)

resp = ""
while True:
    try:
        s.settimeout(0.5)
        c = s.recv(4096).decode('latin1', errors='replace')
        if c: resp += c
        else: break
    except: break

clean = re.sub(r'[\x1b\x9b]\[?[0-9;]*[a-zA-Z]', '', resp)
print("AFTER CONFIRMING TIME:\n", clean)

p.terminate()
