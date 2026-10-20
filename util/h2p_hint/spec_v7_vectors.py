"""Generate fixed vectors with a bit-by-bit oracle independent of production folding."""
import hashlib
import json
from pathlib import Path


def oracle_component(value):
    index = tag = 0
    for bit in range(value.bit_length()):
        if value & (1 << bit):
            index ^= 1 << (bit % 11)
            tag ^= 1 << (bit % 17)
            shifted = bit % 16 + 2
            if shifted < 17:
                tag ^= 1 << shifted
    return (tag << 11) | index


def generate():
    rows = []
    histories = [(0, 0, 0, 0)]
    for context in range(4):
        for bit in range(16 if context == 3 else 128):
            h = [0] * 4
            h[context] = 1 << bit
            histories.append(tuple(h))
    histories += [(0x123456789abcdef00123456789abcdef, (1 << 128)-1,
                   0xaaaaaaaa55555555aaaaaaaa55555555, 0x81a7)]
    for pc in (0x1000, 0x1f2582, 0xffff800000001000):
        word = (pc >> 1) & ((1 << 47)-1)
        for h in histories:
            components = list(map(oracle_component, h))
            for mask in range(16):
                key = oracle_component(word)
                for bit, c in enumerate(components):
                    if mask & (1 << bit):
                        key ^= c
                rows.append(dict(pc=hex(pc), ith_low128=hex(h[0]), call_low128=hex(h[1]),
                                 ret_low128=hex(h[2]), lhr_low16=hex(h[3]),
                                 components=components, mask=mask, key=key,
                                 set=key & 2047, tag=key >> 11))
    from . import context_v5_contract as contract
    root = Path(__file__).parent
    image = contract.pack_image(json.loads((root/'hint-context-v7-metadata.json').read_text()))
    return {'version': 7, 'context_encoding': contract.CONTEXT_ENCODING, 'oracle': 'independent bit parity routing, no production fold/hash calls',
            'hash': rows, 'sample_image_sha256': hashlib.sha256(image).hexdigest()}


if __name__ == '__main__':
    (Path(__file__).parent/'hint-context-v7-vectors.json').write_text(
        json.dumps(generate(), separators=(',', ':'))+'\n')
