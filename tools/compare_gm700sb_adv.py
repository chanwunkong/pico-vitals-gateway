#!/usr/bin/env python3
"""
比對 GM700SB「閒置中」跟「剛量測完」兩份序列埠 log 的 raw_adv hex dump，
找出廣播內容有沒有差異。

用法：
    python tools/compare_gm700sb_adv.py idle.log after_measure.log

log 檔案內容就是直接把序列埠輸出整段存下來（例如 VSCode Serial Monitor
的存檔功能），腳本會自動抓出所有 "raw_adv(N bytes)=<hex>" 這種格式的行，
依裝置位址（addr=...）分組，比較兩份檔案裡相同位址的 hex dump 是否一致。
"""
import re
import sys
from collections import defaultdict

LINE_RE = re.compile(
    r"addr=(?P<addr>[0-9A-Fa-f:]+).*?raw_adv\((?P<len>\d+) bytes\)=(?P<hex>[0-9A-Fa-f]+)"
)


def parse_log(path):
    """回傳 {addr: [hex_str, ...]}，依出現順序保留每一筆。"""
    samples = defaultdict(list)
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE_RE.search(line)
            if not m:
                continue
            addr = m.group("addr").upper()
            samples[addr].append(m.group("hex").lower())
    return samples


def diff_hex(a, b):
    """回傳 a/b 逐 byte 比對的差異描述，長度不同也一併標出來。"""
    if len(a) != len(b):
        return f"長度不同：閒置 {len(a)//2} bytes vs 量測後 {len(b)//2} bytes"
    diffs = []
    for i in range(0, len(a), 2):
        ba, bb = a[i:i+2], b[i:i+2]
        if ba != bb:
            diffs.append(f"byte[{i//2}]: {ba} -> {bb}")
    return "; ".join(diffs) if diffs else None


def main():
    if len(sys.argv) != 3:
        print(f"用法: {sys.argv[0]} <idle.log> <after_measure.log>")
        sys.exit(1)

    idle_path, after_path = sys.argv[1], sys.argv[2]
    idle = parse_log(idle_path)
    after = parse_log(after_path)

    if not idle:
        print(f"警告：{idle_path} 裡沒有抓到任何 raw_adv 行，確認 log 內容/格式是否正確。")
    if not after:
        print(f"警告：{after_path} 裡沒有抓到任何 raw_adv 行，確認 log 內容/格式是否正確。")

    all_addrs = sorted(set(idle) | set(after))
    if not all_addrs:
        sys.exit(1)

    any_diff = False
    for addr in all_addrs:
        idle_samples = idle.get(addr, [])
        after_samples = after.get(addr, [])
        print(f"\n=== 裝置 {addr} ===")
        print(f"  閒置樣本數: {len(idle_samples)}, 量測後樣本數: {len(after_samples)}")

        if idle_samples:
            # 先看閒置樣本彼此是否一致（確認基準穩定，不是本來就會變動的欄位）
            idle_unique = set(idle_samples)
            if len(idle_unique) > 1:
                print(f"  [注意] 閒置樣本本身就有 {len(idle_unique)} 種不同內容，"
                      f"代表某些欄位平常就會變動，比對量測後差異時要排除這些欄位。")
                for h in idle_unique:
                    print(f"    idle variant: {h}")

        if not idle_samples or not after_samples:
            print("  跳過比對（缺少其中一邊的樣本）")
            continue

        baseline = idle_samples[-1]  # 用最後一筆閒置樣本當基準
        for i, h in enumerate(after_samples):
            d = diff_hex(baseline, h)
            if d:
                any_diff = True
                print(f"  [差異] 量測後第 {i+1} 筆 vs 閒置基準: {d}")
            else:
                print(f"  [相同] 量測後第 {i+1} 筆跟閒置基準完全一樣")

    print("\n" + "=" * 50)
    if any_diff:
        print("結論：偵測到廣播內容差異，代表有機會不連線就分辨「有新資料」——"
              "檢查上面標出來的 byte 位置，確認是不是穩定重現（多測幾次），"
              "再考慮改成比照 FORA 系列，掃到就直接連線。")
    else:
        print("結論：閒置跟量測後的廣播內容完全一樣，代表目前的判斷（廣播內容看不出"
              "新資料）成立，維持現有手動/定時同步視窗的設計即可。")


if __name__ == "__main__":
    main()
