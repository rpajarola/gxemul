import subprocess, socket, time

log_file = open('/tmp/gxemul_nvram.log', 'w')
p = subprocess.Popen(['./gxemul', '-e', 'jensen', '-x', '-M', '64', '-d', '0:experiments/nt/nt_disk.img', '-d', '4:/tmp/nt31_alpha.iso', 'media/rom/jensen_v1_3.rom'], stdout=log_file, stderr=subprocess.STDOUT)

time.sleep(1)
s = socket.socket()
s.connect(('127.0.0.1', 4000))
s.settimeout(0.5)

print("Waiting for boot...", flush=True)
t0 = time.time()
while time.time() - t0 < 35:
    try:
        data = s.recv(4096)
        if b'Use the arrow keys to select, then press Enter.' in data:
            print("Boot menu ready!", flush=True)
            break
    except socket.timeout:
        pass

time.sleep(1.0)

def send_key(k, delay=0.5):
    s.sendall(k)
    time.sleep(delay)

print("Navigating to Supplementary menu...", flush=True)
send_key(b"\x9bB")
send_key(b"\x9bB")
send_key(b"\x9bB")
send_key(b"\r", delay=2.0)

print("Navigating to 'Set up the system...'...", flush=True)
send_key(b"\x9bB")
send_key(b"\x9bB")
send_key(b"\r", delay=2.0)

print("Selecting 'Set default environment variables'...", flush=True)
send_key(b"\x9bB")
send_key(b"\r", delay=2.0)

print("Selecting 'Set default configuration'...", flush=True)
send_key(b"\x9bB")
send_key(b"\r", delay=2.0)

print("Selecting 'Supplementary menu, and save changes...'...", flush=True)
for i in range(9):
    send_key(b"\x9bB", delay=0.3)
send_key(b"\r", delay=5.0)

print("Waiting 10 seconds for Flash to write...", flush=True)
time.sleep(10.0)

p.terminate()
p.wait()
print("Done!", flush=True)
