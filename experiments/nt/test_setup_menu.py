import subprocess, socket, time, re, sys, os
from vt100 import VT100

sys.stdout.reconfigure(line_buffering=True)

p = subprocess.Popen(['./gxemul', '-x', '-e', 'jensen', '-M', '64', 'media/rom/jensen_v1_3.rom'],
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

port = None
t0 = time.time()
while time.time() - t0 < 5:
    line = p.stdout.readline()
    if not line:
        break
    m = re.search(r'telnet localhost (\d+): tty0', line)
    if m:
        port = int(m.group(1))
        break

if not port:
    p.kill()
    sys.exit("Failed to find port")

print(f"Connected to port {port}")
s = socket.socket()
s.connect(('127.0.0.1', port))
vt = VT100()

def drain(pause=0.3):
    time.sleep(pause)
    out = ""
    while True:
        try:
            s.settimeout(0.2)
            d = s.recv(4096).decode('latin1', errors='replace')
            if not d: break
            out += d
            vt.feed(d)
        except socket.timeout:
            break
    return out

def send_key(k, delay=0.3):
    s.sendall(k)
    time.sleep(delay)
    drain(0.1)

# Abort mem test
for _ in range(8):
    s.sendall(b" ")
    time.sleep(0.15)

t0 = time.time()
while time.time() - t0 < 25:
    s.settimeout(0.5)
    try:
        d = s.recv(4096).decode('latin1', errors='replace')
        if d:
            vt.feed(d)
            if "WARNING:" in vt.render() or "Use the arrow keys" in vt.render():
                break
    except socket.timeout:
        pass

print("Boot completed. Current screen:")
print(vt.render()[:600])

if "WARNING:" in vt.render():
    print("Dismissing warning...")
    send_key(b"\r", delay=1.0)
    drain(0.5)

print("\n--- Moving to Supplementary Menu ---")
send_key(b"\x9bB", delay=0.3)
send_key(b"\x9bB", delay=0.3)
send_key(b"\x9bB", delay=0.3)
send_key(b"\r", delay=1.0)
drain(0.5)
print(vt.render()[:600])

print("\n--- Moving to 'Set up the system...' ---")
send_key(b"\x9bB", delay=0.3)
send_key(b"\x9bB", delay=0.3)
send_key(b"\r", delay=1.0)
drain(0.5)
print(vt.render()[:600])

p.kill()
p.wait()
