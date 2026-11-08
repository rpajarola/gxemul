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

print(">>> Entering Supplementary menu... <<<")
s.sendall(b"\x9bB\x9bB\x9bB\r")
time.sleep(1.0)

print(">>> Entering 'Set up the system...' <<<")
s.sendall(b"\x9bB\x9bB\r")
time.sleep(1.0)

print(">>> 1. Set system time <<<")
s.sendall(b"\r")  # Enter into Set system time
time.sleep(1.0)
s.sendall(b"\r")  # Confirm time
time.sleep(1.0)

print(">>> 2. Set default environment variables <<<")
s.sendall(b"\x9bB\r")
time.sleep(1.5)

print(">>> 3. Set default configuration <<<")
s.sendall(b"\x9bB\x9bB\r")
time.sleep(2.0)

print(">>> Returning to Supplementary menu... <<<")
# In Setup menu:
# Item 0: Set system time
# Item 1: Set default environment variables
# Item 2: Set default configuration
# Item 3: Manage boot selection menu...
# Item 4: Help...
# Item 5: Supplementary menu...
s.sendall(b"\x9bB\x9bB\x9bB\x9bB\x9bB\r")
time.sleep(1.5)

print(">>> Selecting 'Install Windows NT from CD-ROM' <<<")
# In Supplementary menu:
# Item 0: Install new firmware
# Item 1: Install Windows NT from CD-ROM
s.sendall(b"\x9bB\r")
time.sleep(2.0)

resp = ""
t_start = time.time()
while time.time() - t_start < 15:
    try:
        s.settimeout(1.0)
        c = s.recv(4096).decode('latin1', errors='replace')
        if c:
            resp += c
            clean_chunk = re.sub(r'[\x1b\x9b]\[?[0-9;]*[a-zA-Z]', '', c)
            sys.stdout.write(clean_chunk)
            sys.stdout.flush()
    except socket.timeout:
        pass

clean = re.sub(r'[\x1b\x9b]\[?[0-9;]*[a-zA-Z]', '', resp)
print("\n=== SCREEN AFTER INSTALL NT ===")
print(clean)
print("===============================")

p.terminate()
