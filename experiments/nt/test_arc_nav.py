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
    # Wait an extra 2 seconds for menu rendering to finish completely
    time.sleep(2.0)
    while True:
        try:
            s.settimeout(0.2)
            d = s.recv(4096).decode('latin1', errors='replace')
            if d: vt.feed(d)
            else: break
        except: break

log_file = open('/tmp/gxemul_nav.log', 'w')
p = subprocess.Popen(['./gxemul', '-e', 'jensen', '-x', '-M', '64', '-d', '0:experiments/nt/nt_disk.img', '-d', '4:/tmp/nt31_alpha.iso', 'media/rom/jensen_v1_3.rom'], stdout=log_file, stderr=subprocess.STDOUT)

time.sleep(1)
s = socket.socket()
s.connect(('127.0.0.1', 4000))
vt = VT100()

print("Waiting for boot menu...")
wait_for_boot(s, vt)
print("=== INITIAL BOOT MENU ===")
print(vt.render())

# Let's test "Run a program": 2 Down arrows + Enter
print("\n>>> Moving down 2 times to 'Run a program' and pressing Enter... <<<")
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

print("\n=== SCREEN AFTER 'Run a program' ===")
print(vt.render())
print("====================================")

p.terminate()
