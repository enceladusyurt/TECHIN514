import serial
import matplotlib.pyplot as plt

port = '/dev/cu.usbserial-XXXX'   # 改成你的串口
baud = 115200

ser = serial.Serial(port, baud)

time_data = []
signal_data = []
peak_x = []
peak_y = []

plt.ion()
fig, ax = plt.subplots()

while True:

    line = ser.readline().decode().strip()

    try:
        t, signal, peak = line.split(",")
        t = int(t)
        signal = int(signal)
        peak = int(peak)
    except:
        continue

    time_data.append(t)
    signal_data.append(signal)

    if peak > 0:
        peak_x.append(t)
        peak_y.append(peak)

    if len(time_data) > 6000:
        break

ax.plot(time_data, signal_data, label="Signal")
ax.scatter(peak_x, peak_y, color='red', label="Detected Peaks")

ax.set_xlabel("Time (ms)")
ax.set_ylabel("Analog Read")
ax.set_title("Pulse Sensor Signal (30s)")
ax.legend()

plt.show()