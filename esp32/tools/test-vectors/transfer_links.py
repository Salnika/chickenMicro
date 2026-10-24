#!/usr/bin/env python3
"""Test vectors for the firmware's Google Authenticator transfer-link reader.

    python3 tools/test-vectors/transfer_links.py

Prints the kTransfer* constants in test/test_logic/test_main.cpp. The protobuf
is built by hand here, apart from the C++ decoder, so the tests do not check the
decoder against itself. Field numbers follow Google Authenticator's
MigrationPayload message:

    MigrationPayload  1 otp_parameters (repeated)  2 version  3 batch_size
                      4 batch_index  5 batch_id
    OtpParameters     1 secret  2 name  3 issuer  4 algorithm (1 SHA1, 2 SHA256,
                      3 SHA512, 4 MD5)  5 digits (1 six, 2 eight)
                      6 type (1 HOTP, 2 TOTP)  7 counter
"""
import base64, urllib.parse

def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F; n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n: return bytes(out)

def key(field, wire): return varint(field << 3 | wire)
def ld(field, data): return key(field, 2) + varint(len(data)) + data
def vi(field, value): return key(field, 0) + varint(value)

def params(secret=None, name=None, issuer=None, algorithm=None, digits=None, type_=None, counter=None, extra=b""):
    out = b""
    if secret is not None: out += ld(1, secret)
    if name is not None: out += ld(2, name.encode())
    if issuer is not None: out += ld(3, issuer.encode())
    if algorithm is not None: out += vi(4, algorithm)
    if digits is not None: out += vi(5, digits)
    if type_ is not None: out += vi(6, type_)
    if counter is not None: out += vi(7, counter)
    return out + extra

def payload(entries, version=1, batch_size=1, batch_index=0, batch_id=12345, extra=b""):
    out = b"".join(ld(1, e) for e in entries)
    return out + vi(2, version) + vi(3, batch_size) + vi(4, batch_index) + vi(5, batch_id) + extra

def link(data, urlsafe=False, pad=True):
    text = (base64.urlsafe_b64encode if urlsafe else base64.b64encode)(data).decode()
    if not pad: text = text.rstrip("=")
    return "otpauth-migration://offline?data=" + urllib.parse.quote(text, safe="")

github = bytes(range(1, 21))                       # 20 bytes
proton = bytes(range(0xA0, 0xA0 + 32))             # 32 bytes
aws = b"\x42" * 10
batch = payload([
    params(github, "alice@example.com", "GitHub", 1, 1, 2),
    params(b"\x01\x02\x03\x04\x05", "Old bank", "", 1, 1, 1, counter=5),
    params(proton, "Proton:bob", "Proton", 2, 2, 2),
    params(b"\x09" * 16, "Legacy", "", 4, 1, 2),
    params(aws, "", "AWS", 0, 0, 0),
    params(b"\x07" * 64, "Société Générale", "", 3, 1, 2),
    params(b"\x07" * 65, "Too long", "", 1, 1, 2),
    params(None, "No secret", "", 1, 1, 2),
    params(b"\x05" * 10, "", "", 1, 1, 2),
    params(b"\x06" * 10, "Seven", "", 1, 3, 2),
], batch_size=3, batch_index=1)
# Fields a newer exporter might add, of every wire type, inside and outside.
unknown = key(9, 1) + b"\x11" * 8 + key(10, 5) + b"\x22" * 4 + vi(11, 300) + ld(12, b"xyz")
tolerant = payload([params(github, "alice", "GitHub", 1, 1, 2, extra=unknown)], extra=unknown)

vectors = {
    "kTransferBatch": link(batch),
    "kTransferUrlSafe": link(batch, urlsafe=True, pad=False),
    "kTransferUnknownFields": link(tolerant),
    "kTransferTruncated": link(batch[:200]),
    "kTransferEmpty": link(payload([])),
}
for name, value in vectors.items():
    print(f'constexpr const char* {name} =\n    "{value}";')
