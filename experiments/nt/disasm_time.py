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
                    pos = chunk.find(b'Use the arrow keys to adjust the time')
                    if pos != -1:
                        print(f'Chunk size: {hex(size)}, str pos: {hex(pos)}')
                        # Search for 32-bit or 64-bit reference to the string
                        # Or search for lda/ldah references
                        # Let's check guest virtual address. Where in 64MB RAM is this?
                        break
                except:
                    pass

p.terminate()
