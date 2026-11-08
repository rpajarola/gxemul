import subprocess, time, struct

p = subprocess.Popen(['./gxemul', '-e', 'jensen', '-x', '-M', '64', '-d', '0:experiments/nt/nt_disk.img', '-d', '4:/tmp/nt31_alpha.iso', 'media/rom/jensen_v1_3.rom'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
time.sleep(12)

with open(f'/proc/{p.pid}/maps') as f:
    maps = f.readlines()

guest_ram_offset = None
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
                    pos = chunk.find(b'EISA configuration')
                    if pos != -1:
                        # In the string table:
                        # Find guest virtual address where this chunk is mapped
                        # Let's search for "ARC Multiboot Alpha AXP" in the same chunk
                        pos_arc = chunk.find(b'ARC Multiboot')
                        print(f'Chunk {hex(start)}-{hex(end)}: pos(EISA)={hex(pos)}, pos(ARC)={hex(pos_arc)}')
                        # Let's search for any references to pos
                        # Usually strings are in the .rdata / .rodata section
                        break
                except:
                    pass

p.terminate()
