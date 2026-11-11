#!/usr/bin/env python3
"""Unpack a UDIF (koly) DMG with a zlib-compressed HFS+ partition into a raw image."""
import plistlib, struct, sys, zlib, os

DMG = '/home/monsler/Загрузки/CotEditor_7.1.3.dmg'
OUT = sys.argv[1] if len(sys.argv) > 1 else '/mnt/media/machoman/work/dmg/ableton_raw.img'

CHUNK_ZERO = 0x00000000
CHUNK_RAW = 0x00000001
CHUNK_ZLIB = 0x80000005

os.makedirs(os.path.dirname(OUT), exist_ok=True)
f = open(DMG, 'rb')
f.seek(-512, 2)
koly = f.read(512)
data_fork_len, = struct.unpack('>Q', koly[32:40])
xml_offset, = struct.unpack('>Q', koly[352:360])
xml_size, = struct.unpack('>I', koly[220:224])
f.seek(data_fork_len)  # in signed dmgs the blkx plist sits between the data fork and the code signature,
# and koly's XMLOffset points at the signature, not the plist
head = f.read(64 * 1024 * 1024)
end = head.find(b'</plist>')
pl = plistlib.loads(head[:end + 8])

blkx_entries = pl['resource-fork']['blkx']
print(f"{len(blkx_entries)} blkx entr(y/ies)")

for entry in blkx_entries:
    d = entry['Data']
    assert d[:4] == b'mish'
    sect_num, sect_cnt, data_off = struct.unpack('>QQQ', d[8:32])
    nchunks, = struct.unpack('>I', d[0xC8:0xCC])
    name = entry.get('CFName', entry.get('Name', '?'))
    print(f"partition {name!r}: sector {sect_num} count {sect_cnt} ({sect_cnt*512/2**30:.2f} GiB), {nchunks} chunks")
    out = open(OUT, 'w+b')
    out.truncate((sect_num + sect_cnt) * 512)
    off = 0xCC
    done = 0
    for i in range(nchunks):
        et, comment, sn, sc, coff, clen = struct.unpack('>IIQQQQ', d[off:off+40])
        off += 40
        if et == 0xFFFFFFFF:
            break
        pos = (sect_num + sn) * 512
        out.seek(pos)
        if et == CHUNK_ZERO or clen == 0:
            pass  # sparse: leave as hole
        elif et == CHUNK_ZLIB:
            f.seek(coff)
            out.write(zlib.decompress(f.read(clen)))
        elif et == CHUNK_RAW:
            f.seek(coff)
            out.write(f.read(clen))
        else:
            raise SystemExit(f"unsupported chunk type {et:#x} at chunk {i}")
        done += sc
        if i % 1000 == 0:
            print(f"  chunk {i}/{nchunks} ({done*512/2**30:.2f} GiB)", flush=True)
    out.close()
print("done:", OUT, os.path.getsize(OUT))
