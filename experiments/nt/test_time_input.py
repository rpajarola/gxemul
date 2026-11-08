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

# Setup menu:
# Item 0: Set system time
print(">>> Selecting 'Set system time' <<<")
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
print("PROMPT BEFORE INPUT:\n", clean)

print(">>> Sending date: 09-24-93 <<<")
s.sendall(b"09-24-93\r")
time.sleep(1.5)

resp2 = ""
while True:
    try:
        s.settimeout(0.5)
        c = s.recv(4096).decode('latin1', errors='replace')
        if c: resp2 += c
        else: break
    except: break

clean2 = re.sub(r'[\x1b\x9b]\[?[0-9;]*[a-zA-Z]', '', resp2)
print("PROMPT AFTER DATE:\n", clean2)

p.terminate()
