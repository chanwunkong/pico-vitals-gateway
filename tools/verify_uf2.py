#!/usr/bin/env python3
"""驗證用：把 .uf2 還原成連續 binary，跟原始 .bin 逐 byte 比對，
確認 bin2uf2.py 產生的檔案內容正確（round-trip 檢查）。"""
import struct
import sys

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
ABSOLUTE_FAMILY_ID = 0xE48BFF57


def main():
    uf2_path, bin_path, base_addr_str = sys.argv[1], sys.argv[2], sys.argv[3]
    base_addr = int(base_addr_str, 0)

    with open(uf2_path, "rb") as f:
        uf2_data = f.read()

    assert len(uf2_data) % 512 == 0, "檔案長度不是 512 的倍數"
    num_uf2_blocks = len(uf2_data) // 512

    pages = {}
    abs_blocks = 0
    for i in range(num_uf2_blocks):
        block = uf2_data[i * 512:(i + 1) * 512]
        magic0, magic1, flags, target_addr, payload_size, block_no, num_blocks, file_size = \
            struct.unpack_from("<8I", block)
        data = block[32:32 + 476]
        magic_end = struct.unpack_from("<I", block, 508)[0]
        assert magic0 == UF2_MAGIC_START0 and magic1 == UF2_MAGIC_START1 and magic_end == UF2_MAGIC_END, \
            f"block {i} magic number 不對"
        if file_size == ABSOLUTE_FAMILY_ID:
            abs_blocks += 1
            continue
        pages[target_addr] = data[:payload_size]

    print(f"共 {num_uf2_blocks} 個 UF2 block，{abs_blocks} 個 abs block，{len(pages)} 個資料頁")

    with open(bin_path, "rb") as f:
        original = f.read()

    reconstructed = bytearray()
    for addr in sorted(pages):
        assert addr == base_addr + len(reconstructed), f"位址不連續：預期 {hex(base_addr + len(reconstructed))}，實際 {hex(addr)}"
        reconstructed += pages[addr]

    reconstructed = bytes(reconstructed[:len(original)])  # 去掉最後一頁的補零
    if reconstructed == original:
        print(f"OK：還原後的 {len(reconstructed)} bytes 跟原始 {bin_path} 完全一致")
    else:
        print("FAIL：內容不一致！")
        for i in range(min(len(reconstructed), len(original))):
            if reconstructed[i] != original[i]:
                print(f"  第一個差異在 offset {i}: uf2={reconstructed[i]:02x} bin={original[i]:02x}")
                break
        sys.exit(1)


if __name__ == "__main__":
    main()
