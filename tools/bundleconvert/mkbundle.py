"""Synthesise UnityFS AssetBundles for testing the on-device converter."""
import struct, lzma, os, sys, random

# ---------------- LZ4 block compressor (simple hash-chain, emits matches) -----
def lz4_compress(src: bytes) -> bytes:
    out = bytearray()
    n = len(src)
    table = {}
    anchor = 0
    i = 0
    seqs = []
    while i < n - 12:
        key = src[i:i+4]
        cand = table.get(key)
        table[key] = i
        if cand is not None and i - cand <= 65535 and src[cand:cand+4] == key:
            # extend
            ml = 4
            while i + ml < n - 5 and src[cand+ml] == src[i+ml] and ml < 65535:
                ml += 1
            seqs.append((anchor, i - anchor, i - cand, ml))
            i += ml
            anchor = i
            continue
        i += 1
    # emit
    for (lit_start, lit_len, offset, match_len) in seqs:
        ml = match_len - 4
        token = (min(lit_len, 15) << 4) | min(ml, 15)
        out.append(token)
        if lit_len >= 15:
            rem = lit_len - 15
            while rem >= 255:
                out.append(255); rem -= 255
            out.append(rem)
        out += src[lit_start:lit_start+lit_len]
        out += struct.pack('<H', offset)
        if ml >= 15:
            rem = ml - 15
            while rem >= 255:
                out.append(255); rem -= 255
            out.append(rem)
    # trailing literals
    lit_len = n - anchor
    token = (min(lit_len, 15) << 4)
    out.append(token)
    if lit_len >= 15:
        rem = lit_len - 15
        while rem >= 255:
            out.append(255); rem -= 255
        out.append(rem)
    out += src[anchor:]
    return bytes(out)

def lzma_compress(src: bytes) -> bytes:
    filt = [{"id": lzma.FILTER_LZMA1, "preset": 6}]
    c = lzma.LZMACompressor(format=lzma.FORMAT_ALONE, filters=filt)
    blob = c.compress(src) + c.flush()
    # alone = 5 props + 8 size + stream; Unity stores 5 props + stream
    return blob[:5] + blob[13:]

COMPRESS = {0: lambda b: b, 1: lzma_compress, 2: lz4_compress, 3: lz4_compress}

# ---------------- SerializedFile synthesis ----------------------------------
def serialized_file(version: int, target: int, payload_len: int, unity="2021.3.16f1") -> bytes:
    body = bytearray()
    if version >= 22:
        head_len = 48
    else:
        head_len = 20
    meta = bytearray()
    meta += unity.encode() + b"\0"
    meta += struct.pack('<i', target)
    meta += b"\x00"                       # m_EnableTypeTree
    meta += bytes(payload_len)            # stand-in for the rest of the metadata+data
    total = head_len + len(meta)
    data_offset = head_len + 16
    metadata_size = len(meta)
    if version >= 22:
        body += struct.pack('>I', 0)            # legacy metadataSize
        body += struct.pack('>I', 0)            # legacy fileSize
        body += struct.pack('>I', version)
        body += struct.pack('>I', 0)            # legacy dataOffset
        body += b'\x00' + b'\x00\x00\x00'       # little-endian + reserved
        body += struct.pack('>I', metadata_size)
        body += struct.pack('>Q', total)
        body += struct.pack('>Q', data_offset)
        body += struct.pack('>Q', 0)
    else:
        body += struct.pack('>I', metadata_size)
        body += struct.pack('>I', total)
        body += struct.pack('>I', version)
        body += struct.pack('>I', data_offset)
        body += b'\x00' + b'\x00\x00\x00'
    body += meta
    assert len(body) == total, (len(body), total)
    return bytes(body)

def resource_node(size: int, seed: int) -> bytes:
    rnd = random.Random(seed)
    return bytes(rnd.randrange(256) for _ in range(size))

# ---------------- Bundle assembly -------------------------------------------
def build(path, *, version=6, compression=2, block_compression=None,
          info_at_end=False, sf_version=17, target=19, block_size=0x2000,
          n_files=2, with_resource=True, payload=4000, sf_bytes=None):
    """sf_bytes, when given, supplies the SerializedFile payloads verbatim
    instead of generating filler ones -- which is how a bundle carrying a real
    Shader asset gets built for the shader-translation tests."""
    if block_compression is None:
        block_compression = compression

    nodes = []
    data = bytearray()
    files = sf_bytes if sf_bytes is not None else [
        serialized_file(sf_version, target, payload + i * 137) for i in range(n_files)]
    for i, sf in enumerate(files):
        nodes.append((len(data), len(sf), 4, f"CAB-{i:032x}"))
        data += sf
    if with_resource:
        res = resource_node(3000, 7)
        nodes.append((len(data), len(res), 0, "CAB-deadbeef.resS"))
        data += res
    data = bytes(data)

    # storage blocks
    blocks = []
    blob = bytearray()
    for off in range(0, len(data), block_size):
        chunk = data[off:off+block_size]
        comp = COMPRESS[block_compression](chunk)
        blocks.append((len(chunk), len(comp), block_compression))
        blob += comp

    info = bytearray()
    info += bytes(16)
    info += struct.pack('>i', len(blocks))
    for (u, c, f) in blocks:
        info += struct.pack('>IIH', u, c, f)
    info += struct.pack('>i', len(nodes))
    for (o, s, f, p) in nodes:
        info += struct.pack('>qqI', o, s, f) + p.encode() + b"\0"
    info = bytes(info)
    cinfo = COMPRESS[compression](info)

    flags = compression | 0x40
    if info_at_end:
        flags |= 0x80

    head = bytearray()
    head += b"UnityFS\0"
    head += struct.pack('>I', version)
    head += b"5.x.x\0"
    head += b"2021.3.16f1\0"
    size_off = len(head)
    head += struct.pack('>Q', 0)
    head += struct.pack('>I', len(cinfo))
    head += struct.pack('>I', len(info))
    head += struct.pack('>I', flags)
    if version >= 7:
        while len(head) % 16 != 0:
            head += b"\0"

    if info_at_end:
        out = bytes(head) + bytes(blob) + cinfo
    else:
        out = bytes(head) + cinfo + bytes(blob)
    out = bytearray(out)
    out[size_off:size_off+8] = struct.pack('>Q', len(out))
    with open(path, 'wb') as f:
        f.write(bytes(out))
    return data, nodes

# ---------------- Verifier ---------------------------------------------------
def read_converted(path):
    raw = open(path, 'rb').read()
    p = raw.index(b"\0") + 1
    assert raw[:7] == b"UnityFS"
    version = struct.unpack_from('>I', raw, p)[0]; p += 4
    e = raw.index(b"\0", p); uver = raw[p:e]; p = e + 1
    e = raw.index(b"\0", p); urev = raw[p:e]; p = e + 1
    size = struct.unpack_from('>Q', raw, p)[0]; p += 8
    cinfo_size = struct.unpack_from('>I', raw, p)[0]; p += 4
    uinfo_size = struct.unpack_from('>I', raw, p)[0]; p += 4
    flags = struct.unpack_from('>I', raw, p)[0]; p += 4
    assert size == len(raw), (size, len(raw))
    assert flags & 0x3F == 0, "output must be uncompressed"
    assert cinfo_size == uinfo_size
    if version >= 7:
        while p % 16 != 0:
            p += 1
    info = raw[p:p+cinfo_size]; p += cinfo_size
    q = 16
    nblocks = struct.unpack_from('>i', info, q)[0]; q += 4
    total = 0
    for _ in range(nblocks):
        u, c, f = struct.unpack_from('>IIH', info, q); q += 10
        assert f == 0 and u == c
        total += u
    nnodes = struct.unpack_from('>i', info, q)[0]; q += 4
    nodes = []
    for _ in range(nnodes):
        o, s, f = struct.unpack_from('>qqI', info, q); q += 20
        e = info.index(b"\0", q); nm = info[q:e].decode(); q = e + 1
        nodes.append((o, s, f, nm))
    data = raw[p:p+total]
    assert len(data) == total, (len(data), total)
    return version, uver, urev, nodes, data

def target_of(data, off, size, sf_version):
    head_len = 48 if sf_version >= 22 else 20
    p = off + head_len
    e = data.index(b"\0", p)
    p = e + 1
    return struct.unpack_from('<i', data, p)[0]
