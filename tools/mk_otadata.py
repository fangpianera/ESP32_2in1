import struct, zlib
# IDF esp_ota_select_entry_t: ota_seq(4) + seq_label[20] + ota_state(4) + crc(4)
seq = 3            # 1=ota_0, 2=ota_1, 3=ota_2 (app_f1)
label = b"\x00" * 20
state = 2          # ESP_OTA_IMG_VALID
body = struct.pack("<I", seq) + label + struct.pack("<I", state)
crc = zlib.crc32(body) & 0xFFFFFFFF
entry = body + struct.pack("<I", crc)
sec0 = entry + b"\xff" * (4096 - 32)
sec1 = b"\xff" * 4096
with open(r"D:\nav_build\otadata_fix.bin", "wb") as f:
    f.write(sec0 + sec1)
print("crc=%08X seq=%d state=%d" % (crc, seq, state))
