import subprocess, socket, time
from vt100 import VT100

log_file = open('/tmp/gxemul_monitor.log', 'w')
p = subprocess.Popen(['./gxemul', '-e', 'jensen', '-x', '-M', '64', '-d', '0:experiments/nt/nt_disk.img', '-d', '4:/tmp/nt31_alpha.iso', 'media/rom/jensen_v1_3.rom'], stdout=log_file, stderr=subprocess.STDOUT)

time.sleep(1)
s = socket.socket()
s.connect(('127.0.0.1', 4000))
vt = VT100()

print("Connected. Collecting console output for 35 seconds...")
t0 = time.time()
last_print = t0
while time.time() - t0 < 35:
    s.settimeout(0.5)
    try:
        d = s.recv(1024).decode('latin1', errors='replace')
        if d:
            vt.feed(d)
    except socket.timeout:
        pass
    if time.time() - last_print > 5.0:
        print(f"[{int(time.time() - t0)}s elapsed]")
        last_print = time.time()

print("\n=== SCREEN AFTER 35s ===")
print(vt.render())
print("========================")

# Now send ESC or Enter to see if warning box goes away or if menu responds
print("\n>>> Sending Enter (\\n) <<<")
s.sendall(b"\n")
time.sleep(2.0)
while True:
    try:
        s.settimeout(0.2)
        d = s.recv(4096).decode('latin1', errors='replace')
        if d: vt.feed(d)
        else: break
    except: break

print("\n=== SCREEN AFTER Enter ===")
print(vt.render())
print("==========================")

p.terminate()
