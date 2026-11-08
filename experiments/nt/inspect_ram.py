import subprocess, time, re

p = subprocess.Popen(['./gxemul', '-e', 'jensen', '-x', '-M', '64', '-d', '0:experiments/nt/nt_disk.img', '-d', '4:/tmp/nt31_alpha.iso', 'media/rom/jensen_v1_3.rom'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
time.sleep(15)

try:
    with open(f'/proc/{p.pid}/maps') as f:
        maps = f.readlines()

    with open(f'/proc/{p.pid}/mem', 'rb', 0) as mem:
        for line in maps:
            parts = line.split()
            addr_range = parts[0]
            perms = parts[1]
            if 'rw' in perms and not parts[-1].endswith('.so') and not '[' in parts[-1]:
                start, end = [int(x, 16) for x in addr_range.split('-')]
                size = end - start
                if size == 64 * 1024 * 1024:
                    print(f'Guest RAM: {hex(start)}-{hex(end)} ({size} bytes)')
                    mem.seek(start)
                    data = mem.read(size)
                    idx = 0
                    while True:
                        pos = data.find(b'Testing NVR', idx)
                        if pos == -1:
                            break
                        print(f'Found "Testing NVR" at RAM offset {hex(pos)}: {data[pos:pos+100]}')
                        idx = pos + 1
                    pos2 = data.find(b'failed')
                    print(f'First "failed" at {hex(pos2)}')
finally:
    p.terminate()
