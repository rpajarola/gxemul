import subprocess, socket, time
from vt100 import VT100

def wait_for_boot(s, vt):
    t0 = time.time()
    buf = ""
    while time.time() - t0 < 45:
        s.settimeout(0.5)
        try:
            d = s.recv(1024).decode('latin1', errors='replace')
            if d:
                vt.feed(d)
                buf += d
                if 'Use the arrow keys to select, then press Enter.' in buf:
                    break
        except socket.timeout:
            pass
    time.sleep(2.0)
    while True:
        try:
            s.settimeout(0.2)
            d = s.recv(4096).decode('latin1', errors='replace')
            if d: vt.feed(d)
            else: break
        except: break

log_file = open('/tmp/gxemul_run_arcinst.log', 'w')
p = subprocess.Popen(['./gxemul', '-e', 'jensen', '-x', '-M', '64', '-d', '0:experiments/nt/nt_disk.img', '-d', '4:/tmp/nt31_alpha.iso', 'media/rom/jensen_v1_3.rom'], stdout=log_file, stderr=subprocess.STDOUT)

time.sleep(1)
s = socket.socket()
s.connect(('127.0.0.1', 4000))
vt = VT100()

print("Waiting for boot menu...")
wait_for_boot(s, vt)
print("=== INITIAL BOOT MENU ===")
print(vt.render())

print("\n>>> Navigating to 'Run a program' <<<")
s.sendall(b"\x9bB")
time.sleep(0.3)
s.sendall(b"\x9bB")
time.sleep(0.3)
s.sendall(b"\n")
time.sleep(2.0)

while True:
    try:
        s.settimeout(0.2)
        d = s.recv(4096).decode('latin1', errors='replace')
        if d: vt.feed(d)
        else: break
    except: break

print("=== SCREEN AT PROGRAM PROMPT ===")
print(vt.render())

cmd = "scsi(0)cdrom(4)fdisk(0)\\alpha\\arcinst.exe\n"
print(f"\n>>> Typing program: {cmd.strip()} <<<")
for ch in cmd:
    s.sendall(ch.encode('latin1'))
    time.sleep(0.05)

print("Waiting 5 seconds for response...")
t0 = time.time()
while time.time() - t0 < 5.0:
    s.settimeout(0.5)
    try:
        d = s.recv(1024).decode('latin1', errors='replace')
        if d:
            vt.feed(d)
            print(d, end='', flush=True)
    except socket.timeout:
        pass

print("\n\n=== SCREEN AFTER RUNNING PROGRAM ===")
print(vt.render())
print("====================================")

p.terminate()
