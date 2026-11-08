import subprocess, socket, time, re

p = subprocess.Popen(['./gxemul', '-e', 'jensen', '-x', '-M', '64', '-d', '0:experiments/nt/nt_disk.img', '-d', '4:/tmp/nt31_alpha.iso', 'media/rom/jensen_v1_3.rom'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

time.sleep(1)
s = socket.socket()
s.connect(('127.0.0.1', 4000))

buf = ""
t0 = time.time()
while time.time() - t0 < 35:
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

print(">>> Navigating to Supplementary menu... <<<", flush=True)
for _ in range(3):
    s.sendall(b"\x9bB")
    time.sleep(0.5)
s.sendall(b"\r")
time.sleep(2.0)

# Drain
while True:
    try:
        s.settimeout(0.2)
        s.recv(4096)
    except:
        break

print(">>> Navigating to 'Set up the system...' <<<", flush=True)
for _ in range(2):
    s.sendall(b"\x9bB")
    time.sleep(0.5)
s.sendall(b"\r")
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
print("\n=== SETUP MENU ITEMS ===", flush=True)
print(clean, flush=True)
print("========================", flush=True)

p.terminate()
p.wait()
