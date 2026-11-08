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
                        # 0x7ca4: lda t12, -0x27f0(gp)
                        # Let's find gp at 0x7ca0
                        # 0x7ca0: ldah gp, 1(ra) where ra is 0x20007ca4
                        ra = 0x20007ca4
                        high = ((ra + 0x8000) & 0xffffffff) + 0x10000
                        # Or better: let's inspect the GOT entry!
                        # What is at 0(sp) when 0x7cbc loads ra?
                        # Wait, at 0x7cbc it does: ldq ra, 0(sp) or ldq t12, ...?
                        # Let's check 0x7cb8 - 0x7cc8:
                        for j in range(0x7ca0, 0x7cd0, 4):
                            w = struct.unpack('<I', chunk[j:j+4])[0]
                            print(f'  0x{0x20000000 + j:08x}: 0x{w:08x}')
                        break
                except:
                    pass

p.terminate()
