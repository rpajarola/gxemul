import struct

# Create MBR + FAT16 System Partition on nt_disk.img
with open("experiments/nt/nt_disk.img", "r+b") as f:
    # Disk geometry: 500 MB = 1024128 sectors
    # Partition 1: 10 MB (20480 sectors) starting at sector 63
    part1_lba = 63
    part1_sectors = 20480 # 10 MB
    
    # MBR
    mbr = bytearray(512)
    # Partition 1 entry at offset 446 (0x1be)
    # Status: 0x80 (active)
    # CHS start: 0/1/1
    # Type: 0x04 (FAT16 <32MB) or 0x06 (FAT16)
    # CHS end: 10/63/32
    mbr[446] = 0x80 # Active
    mbr[447] = 1    # Head
    mbr[448] = 1    # Sector
    mbr[449] = 0    # Cylinder
    mbr[450] = 0x04 # Type: FAT16 < 32MB
    mbr[451] = 63   # End Head
    mbr[452] = 32   # End Sector
    mbr[453] = 10   # End Cylinder
    struct.pack_into("<II", mbr, 454, part1_lba, part1_sectors)
    
    # MBR Signature
    mbr[510] = 0x55
    mbr[511] = 0xaa
    
    f.seek(0)
    f.write(mbr)
    
    # Now create FAT16 BPB at part1_lba * 512
    bs = bytearray(512)
    bs[0:3] = b"\xeb\x3c\x90" # Jump instruction
    bs[3:11] = b"MSDOS5.0"   # OEM Name
    
    bytes_per_sec = 512
    sec_per_cluster = 4       # 2 KB clusters
    res_sectors = 1
    num_fats = 2
    root_entries = 512        # 32 sectors for root dir
    total_sec_16 = part1_sectors
    media = 0xf8              # Fixed disk
    sec_per_fat = 16
    sec_per_track = 63
    num_heads = 16
    hidden_sec = part1_lba
    
    struct.pack_into("<HBHBHHBHHHII", bs, 11,
                     bytes_per_sec,
                     sec_per_cluster,
                     res_sectors,
                     num_fats,
                     root_entries,
                     total_sec_16,
                     media,
                     sec_per_fat,
                     sec_per_track,
                     num_heads,
                     hidden_sec,
                     0) # total_sec_32 = 0
    
    bs[36] = 0x80 # Drive number
    bs[38] = 0x29 # Extended boot signature
    struct.pack_into("<I", bs, 39, 0x12345678) # Volume ID
    bs[43:54] = b"SYSTEM     " # Volume label
    bs[54:62] = b"FAT16   "   # File system type
    
    bs[510] = 0x55
    bs[511] = 0xaa
    
    f.seek(part1_lba * 512)
    f.write(bs)
    
    # Write FAT 1 and FAT 2
    fat = bytearray(sec_per_fat * 512)
    # First two FAT entries: media byte (0xf8) and 0xffff
    fat[0] = 0xf8
    fat[1] = 0xff
    fat[2] = 0xff
    fat[3] = 0xff
    
    fat1_offset = (part1_lba + res_sectors) * 512
    f.seek(fat1_offset)
    f.write(fat)
    
    fat2_offset = (part1_lba + res_sectors + sec_per_fat) * 512
    f.seek(fat2_offset)
    f.write(fat)
    
    # Zero the root directory
    root_offset = (part1_lba + res_sectors + num_fats * sec_per_fat) * 512
    root_size = (root_entries * 32)
    f.seek(root_offset)
    f.write(b"\x00" * root_size)
    
    f.flush()

print(f"Successfully initialized 10MB FAT16 System Partition at sector {part1_lba} on nt_disk.img")
