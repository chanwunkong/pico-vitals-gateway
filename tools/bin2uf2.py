#!/usr/bin/env python3
"""
不依賴 picotool 的 .bin -> .uf2 轉換器。

背景：這台機器上 CMake 自動下載/建置出來的 picotool.exe（v2.3.0，GNU 16.1.0
Windows Release）執行 `picotool uf2 convert` 時穩定 Segmentation fault（不管
輸入是 .elf 還是 .bin、有沒有指定 --family，一律當機；但 `picotool help`／
`picotool version`／`arm-none-eabi-g++` link／objdump／objcopy 全部正常，
證實問題精確定位在 picotool 的 uf2 convert 這個子指令本身，不是資源衝突或
編譯出來的韌體本身有問題）。改寫這個小工具繞過，邏輯照抄 picotool 原始碼
（build_pico2w/_deps/picotool-src/elf2uf2/elf2uf2.cpp 的
bin2uf2()/gen_abs_block()）跟格式定義
（C:\\Users\\307\\pico-sdk\\src\\common\\boot_uf2_headers\\include\\boot\\uf2.h）。

已知簡化：picotool 實際吃 .elf 時走的是 elf2uf2()，會對「非最後一個」flash
sector 補零 padding 頁對齊 4096 bytes 邊界（讓 bootrom 抹寫時的 sector 計算
正確）。這裡改吃 objcopy -Obinary 產生的 .bin（本來就是從 flash 起始位置
連續、無空隙的完整影像，objcopy 已經把任何 segment 間的間隙補零），順序切
256 bytes 一頁效果等價，不需要另外補 padding 頁。

用法：
    python tools/bin2uf2.py build2/pico_gateway.bin build2/pico_gateway.uf2 --family rp2040
    python tools/bin2uf2.py build_pico2w/pico_gateway.bin build_pico2w/pico_gateway.uf2 \
        --family rp2350-arm-s --abs-block
"""
import argparse
import struct

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30

UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000
UF2_FLAG_EXTENSION_FLAGS_PRESENT = 0x00008000

# 跟 boot/uf2.h 逐項對應
FAMILY_IDS = {
    "rp2040": 0xE48BFF56,
    "absolute": 0xE48BFF57,
    "data": 0xE48BFF58,
    "rp2350-arm-s": 0xE48BFF59,
    "rp2350-riscv": 0xE48BFF5A,
    "rp2350-arm-ns": 0xE48BFF5B,
}

UF2_EXTENSION_RP2_IGNORE_BLOCK = 0x9957E304

FLASH_START = 0x10000000
PAGE_SIZE = 256  # UF2_PAGE_SIZE，跟 elf2uf2.h 的 LOG2_PAGE_SIZE=8 一致
DATA_FIELD_SIZE = 476  # struct uf2_block 的 data[476]
# picotool main.cpp 預設值：0x11000000 - UF2_PAGE_SIZE
DEFAULT_ABS_BLOCK_LOC = 0x11000000 - PAGE_SIZE


def pack_block(flags, target_addr, payload_size, block_no, num_blocks, file_size, data, magic_end=UF2_MAGIC_END):
    assert len(data) == DATA_FIELD_SIZE, f"data field 必須是 {DATA_FIELD_SIZE} bytes，實際 {len(data)}"
    return struct.pack(
        "<8I476sI",
        UF2_MAGIC_START0, UF2_MAGIC_START1, flags, target_addr,
        payload_size, block_no, num_blocks, file_size,
        data, magic_end,
    )


def gen_abs_block(abs_block_loc):
    """對應 elf2uf2.cpp gen_abs_block()：RP2350-E10 errata fix 用的標記 block。"""
    data = bytearray(DATA_FIELD_SIZE)
    data[0:PAGE_SIZE] = b"\xef" * PAGE_SIZE
    struct.pack_into("<I", data, PAGE_SIZE, UF2_EXTENSION_RP2_IGNORE_BLOCK)
    flags = UF2_FLAG_FAMILY_ID_PRESENT | UF2_FLAG_EXTENSION_FLAGS_PRESENT
    return pack_block(flags, abs_block_loc, PAGE_SIZE, 0, 2, FAMILY_IDS["absolute"], bytes(data))


def bin2uf2(data, base_addr, family_id, abs_block_loc=0):
    pages = []
    offset = 0
    while offset < len(data):
        chunk = data[offset:offset + PAGE_SIZE]
        if len(chunk) < PAGE_SIZE:
            chunk = chunk + b"\x00" * (PAGE_SIZE - len(chunk))
        pages.append(chunk)
        offset += PAGE_SIZE

    out = bytearray()
    if abs_block_loc:
        out += gen_abs_block(abs_block_loc)

    num_blocks = len(pages)
    for i, page_data in enumerate(pages):
        target_addr = base_addr + i * PAGE_SIZE
        data_field = page_data + b"\x00" * (DATA_FIELD_SIZE - PAGE_SIZE)
        out += pack_block(UF2_FLAG_FAMILY_ID_PRESENT, target_addr, PAGE_SIZE, i, num_blocks,
                           family_id, data_field)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input_bin")
    ap.add_argument("output_uf2")
    ap.add_argument("--family", choices=sorted(FAMILY_IDS), default="rp2040")
    ap.add_argument("--base-addr", type=lambda x: int(x, 0), default=FLASH_START,
                     help="預設 0x10000000（flash XIP base，RP2040/RP2350 通用）")
    ap.add_argument("--abs-block", action="store_true",
                     help="RP2350-E10 errata fix：加一個絕對位置標記 block（只給 RP2350 用）")
    ap.add_argument("--abs-block-loc", type=lambda x: int(x, 0), default=DEFAULT_ABS_BLOCK_LOC)
    args = ap.parse_args()

    with open(args.input_bin, "rb") as f:
        data = f.read()

    abs_block_loc = args.abs_block_loc if args.abs_block else 0
    uf2_data = bin2uf2(data, args.base_addr, FAMILY_IDS[args.family], abs_block_loc)

    with open(args.output_uf2, "wb") as f:
        f.write(uf2_data)

    note = "，含 1 個 abs block" if abs_block_loc else ""
    print(f"寫入 {args.output_uf2}：{len(uf2_data)} bytes ({len(uf2_data)//512} blocks{note})")


if __name__ == "__main__":
    main()
