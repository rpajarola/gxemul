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
    drain(s, vt, 0.5)

def drain(s, vt, pause=0.5):
    time.sleep(pause)
    while True:
        try:
            s.settimeout(0.2)
            d = s.recv(4096).decode('latin1', errors='replace')
            if d: vt.feed(d)
            else: break
        except: break

def send_keys(s, keys, delay=0.05):
    for ch in keys:
        if isinstance(ch, int):
            s.sendall(bytes([ch]))
        elif isinstance(ch, str):
            s.sendall(ch.encode('latin1'))
        elif isinstance(ch, bytes):
            s.sendall(ch)
        time.sleep(delay)

log_file = open('/tmp/gxemul_run_ecu.log', 'w')
p = subprocess.Popen(['./gxemul', '-e', 'jensen', '-x', '-M', '64', '-d', '0:experiments/nt/nt_disk.img', '-d', '4:/tmp/nt31_alpha.iso', '-d', '5:/tmp/ecu.iso', 'media/rom/jensen_v1_3.rom'], stdout=log_file, stderr=subprocess.STDOUT)

time.sleep(1)
s = socket.socket()
s.connect(('127.0.0.1', 4000))
vt = VT100()

print("Waiting for boot menu...")
wait_for_boot(s, vt)

print("\n>>> Navigating to 'Run a program' <<<")
s.sendall(b"\x9bB")
time.sleep(0.3)
s.sendall(b"\x9bB")
time.sleep(0.3)
s.sendall(b"\n")
time.sleep(2.0)
drain(s, vt, 0.5)

cmd = "scsi(0)cdrom(5)fdisk(0)\\cf.exe\n"
print(f"\n>>> Running {cmd.strip()} <<<")
send_keys(s, cmd)

print("Collecting output for 10 seconds...")
t0 = time.time()
while time.time() - t0 < 10.0:
    s.settimeout(0.5)
    try:
        d = s.recv(1024).decode('latin1', errors='replace')
        if d:
            vt.feed(d)
    except socket.timeout:
        pass

drain(s, vt, 0.5)
print("\n=== SCREEN AFTER RUNNING CF.EXE ===")
print(vt.render())
print("===================================")

p.terminate()
