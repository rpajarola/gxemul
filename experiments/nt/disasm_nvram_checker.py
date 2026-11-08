import subprocess, time, struct

p = subprocess.Popen(['./gxemul', '-e', 'jensen', '-x', '-M', '64', '-d', '0:experiments/nt/nt_disk.img', '-d', '4:/tmp/nt31_alpha.iso', 'media/rom/jensen_v1_3.rom'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
time.sleep(12)

with open(f'/proc/{p.pid}/maps') as f:
    maps = f.readlines()

with open(f'/proc/{p.pid}/mem', 'rb', 0) as mem:
    for line in maps:
        parts = line.split()
        if 'rw' in parts[1] and not parts[-1].endswith('.so') and '[' not in parts[-1]:
            start, end = [int(x, 16) for x in parts[0].split('-')]
            size = end - start
            if size > 65536:
                try:
                    mem.seek(start)
                    chunk = mem.read(size)
                    pos = chunk.find(b'The problem areas are:')
                    if pos != -1:
                        # Dump from 0x7c00 to 0x7e50
                        for j in range(0x7c00, 0x7e50, 4):
                            w = struct.unpack('<I', chunk[j:j+4])[0]
                            print(f'  0x{0x20000000 + j:08x}: 0x{w:08x}')
                        break
                except:
                    pass

p.terminate()
