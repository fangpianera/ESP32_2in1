import serial, time, sys

port = "COM5"
try:
    s = serial.Serial(port, 115200, timeout=0.15)
except Exception as e:
    print("open fail:", e); sys.exit(1)

# ESP32-S3 原生 USB：DTR/RTS 组合触发复位
s.setDTR(False); s.setRTS(True)   # EN=0, IO0=1 -> 复位
time.sleep(0.15)
s.setRTS(False)                  # 释放 -> 正常启动
time.sleep(0.2)

buf = b""
t0 = time.time()
while time.time() - t0 < 4.0:
    chunk = s.read(8192)
    if chunk:
        buf += chunk
s.close()
open(r"D:\backup_fw\sel_boot.log", "wb").write(buf)
print("bytes=%d" % len(buf))
print(buf.decode("utf-8", "replace")[:4000])
