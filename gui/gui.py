import re
import serial
import threading
from collections import deque
import numpy as np
import pyqtgraph as pg
from pyqtgraph.Qt import QtCore
from pyqtgraph import LabelItem
import winsound
import pandas as pd
import os
import glob
import time

# config
PORT = 'COM3'
BAUDRATE = 115200
FS = 256
WINDOW_SEC = 10
WINDOW_SIZE = FS * WINDOW_SEC
UPDATE_HZ = 30
MAX_WINDOWS_PER_FILE = 50

REFRACTORY_SEC = 0.3
MA_WIN_SEC = 0.15
MA_WIN = int(MA_WIN_SEC * FS)

AFIB_ENTER_THRESHOLD = 0.8
AFIB_EXIT_THRESHOLD = 0.2
AFIB_PROB_BUFFER_SIZE = 10

# regexes to detect different data format
# llm-generated cause i'm really bad at regex
# but it works perfectly fine
ECG_RE = re.compile(r'^\[\d+\s*ms\]\s*ECG=')
FLOAT_RE = re.compile(r'^[-+]?\d*\.\d+$')
INFER_PATTERNS = [
    re.compile(r'^Sinus\s*=\s*([-+]?[0-9]*\.?[0-9]+),\s*AFib\s*=\s*([-+]?[0-9]*\.?[0-9]+)'),
    re.compile(r'^Sinus\s*prob:\s*([-+]?[0-9]*\.?[0-9]+),\s*AFib\s*prob:\s*([-+]?[0-9]*\.?[0-9]+)$'),
]

ecg_buf = np.zeros(WINDOW_SIZE, dtype=float)
write_idx = 0
count = 0
lock = threading.Lock()

disp_buf = np.zeros(WINDOW_SIZE, dtype=float)
disp_idx = 0

infer_afib = None
infer_sinus = None
infer_label = None

afib_probs = deque(maxlen=AFIB_PROB_BUFFER_SIZE)
afib_state = "SINUS"

win = None
ecg_curve = None
fft_curve = None
text_label = None
logo_label = None

save_dir = "ecg_windows"
os.makedirs(save_dir, exist_ok=True)
current_file_idx = 0
current_file_rows = 0

# real-time QRS detection
prev_sample = 0.0
diff_buf = deque(maxlen=MA_WIN)
int_buf = deque(maxlen=FS)
running_mean = 0.0
running_std = 1.0
last_qrs_time = 0
beat_times = deque(maxlen=20)

# save ecg data to csv
def init_csv_index():
    global current_file_idx, current_file_rows

    csv_files = sorted(glob.glob(os.path.join(save_dir, "ecg_windows_*.csv")))
    if not csv_files:
        current_file_idx = 0
        current_file_rows = 0
        return

    last_file = csv_files[-1]
    df = pd.read_csv(last_file)
    num_rows = len(df)

    if num_rows < MAX_WINDOWS_PER_FILE:
        current_file_idx = int(os.path.splitext(os.path.basename(last_file))[0].split("_")[-1])
        current_file_rows = num_rows
    else:
        current_file_idx = int(os.path.splitext(os.path.basename(last_file))[0].split("_")[-1]) + 1
        current_file_rows = 0

def get_current_csv_path():
    return os.path.join(save_dir, f"ecg_windows_{current_file_idx}.csv")

def save_window_to_csv(window_data, real_label, pred_label, prob_afib, prob_sinus):
    global current_file_idx, current_file_rows

    row = list(window_data) + [real_label, pred_label, prob_afib, prob_sinus]
    columns = [f"ecg_{i}" for i in range(WINDOW_SIZE)] + [
        "real_label", "pred_label", "prob_afib", "prob_sinus"
    ]

    df_row = pd.DataFrame([row], columns=columns)
    csv_path = get_current_csv_path()
    write_header = not os.path.exists(csv_path)

    df_row.to_csv(
        csv_path,
        mode='a',
        header=write_header,
        index=False
    )

    current_file_rows += 1
    print(f"Saved window to: {csv_path} (rows={current_file_rows})")

    if current_file_rows >= MAX_WINDOWS_PER_FILE:
        current_file_idx += 1
        current_file_rows = 0

def play_beep(freq, duration_ms=150):
    threading.Thread(target=winsound.Beep,
                     args=(int(freq), int(duration_ms)),
                     daemon=True).start()

def parse_inference_line(line: str):
    for pat in INFER_PATTERNS:
        m = pat.match(line)
        if m:
            return float(m.group(1)), float(m.group(2))
    return None, None

def serial_reader():
    global write_idx, count, disp_idx, infer_afib, infer_sinus, infer_label

    ser = serial.Serial(PORT, BAUDRATE, timeout=1)

    while True:
        raw = ser.readline().decode('ascii', errors='ignore').strip()
        if not raw:
            continue

        s, a = parse_inference_line(raw)
        if a is not None:
            with lock:
                infer_afib = a
                infer_sinus = s
                infer_label = "afib" if a > s else "sinus"
                afib_probs.append(a)
            continue

        if ECG_RE.match(raw):
            try:
                v = float(raw.split('=')[-1].split()[0])
            except:
                continue
            append_ecg_sample(v)
            continue

        if FLOAT_RE.match(raw):
            v = float(raw)
            append_ecg_sample(v)
            continue

def append_ecg_sample(v):
    global write_idx, count, disp_idx
    global prev_sample, diff_buf, int_buf, running_mean, running_std, last_qrs_time, beat_times

    with lock:
        ecg_buf[write_idx] = v
        write_idx = (write_idx + 1) % WINDOW_SIZE
        count = min(count + 1, WINDOW_SIZE)

    disp_buf[disp_idx] = v
    disp_idx = (disp_idx + 1) % WINDOW_SIZE

    diff = v - prev_sample
    prev_sample = v

    sq = diff ** 2
    diff_buf.append(sq)

    if len(diff_buf) == MA_WIN:
        integrated = np.mean(diff_buf)
        int_buf.append(integrated)
        
        if len(int_buf) >= MA_WIN:
            running_mean = np.mean(int_buf)
            running_std = np.std(int_buf)
        else:
            running_mean = 0
            running_std = 1

        threshold = running_mean + 0.5 * running_std
        now = time.time()
        if integrated > threshold and (now - last_qrs_time) > REFRACTORY_SEC:
            play_beep(600, 220)
            last_qrs_time = now
            beat_times.append(now)

    if count == WINDOW_SIZE:
        with lock:
            snapshot = np.roll(ecg_buf, -write_idx).copy()
            pred_label = infer_label if infer_label is not None else "unknown"
            prob_afib = infer_afib if infer_afib is not None else np.nan
            prob_sinus = infer_sinus if infer_sinus is not None else np.nan

        save_window_to_csv(
            snapshot,
            real_label="sinus",
            pred_label=pred_label,
            prob_afib=prob_afib,
            prob_sinus=prob_sinus
        )
        count = 0

def determine_afib_state():
    global afib_state

    if len(afib_probs) < AFIB_PROB_BUFFER_SIZE:
        return "SINUS"

    mean_afib = np.mean(afib_probs)

    if afib_state == "SINUS":
        if mean_afib > AFIB_ENTER_THRESHOLD:
            afib_state = "AFIB"
            play_beep(1500, 500)
    else:
        if mean_afib < AFIB_EXIT_THRESHOLD:
            afib_state = "SINUS"

    return afib_state

def compute_heart_rate_from_beats():
    if len(beat_times) < 2:
        return None
    rr_intervals = np.diff(beat_times)
    mean_rr = np.mean(rr_intervals)
    hr = 60.0 / mean_rr if mean_rr > 0 else None
    return hr

def setup_ui():
    global win, ecg_curve, fft_curve, text_label, logo_label

    app = pg.mkQApp("Tinycardia ECG Monitor")
    win = pg.GraphicsLayoutWidget(show=True)
    win.setWindowTitle("Tinycardia Monitor")

    win.ci.layout.setColumnStretchFactor(0, 2)
    win.ci.layout.setColumnStretchFactor(1, 1)

    logo_label = LabelItem(justify='center')
    logo_label.setText("<div style='font-size:48pt; color:white;'><b>TinyCardia</b></div>")
    win.addItem(logo_label, row=0, col=0, colspan=2)

    p1 = win.addPlot(row=1, col=0, rowspan=3, title="ECG (mV)")
    p1.showGrid(x=True, y=True, alpha=0.3)
    ecg_curve = p1.plot(pen=pg.mkPen('g', width=3))

    p2 = win.addPlot(row=4, col=0, title="FFT")
    p2.showGrid(x=True, y=True, alpha=0.3)
    fft_curve = p2.plot(pen=pg.mkPen('m', width=1.5))
    p2.setXRange(0, 120, padding=0)

    text_label = LabelItem(justify='left')
    win.addItem(text_label, row=1, col=1, rowspan=4)

    return app

def update_gui():
    global infer_afib, infer_sinus

    if ecg_curve is not None:
        ecg_curve.setData(disp_buf)

    fft_data = np.abs(np.fft.rfft(disp_buf))
    fft_freqs = np.fft.rfftfreq(len(disp_buf), 1/FS)
    if fft_curve is not None:
        fft_curve.setData(fft_freqs, fft_data)

    afib_status = determine_afib_state()

    hr = compute_heart_rate_from_beats()

    with lock:
        a = infer_afib
        s = infer_sinus

    if a is not None and s is not None:
        if a > s:
            raw_label = "AFIB"
            color = "red"
            confidence = a
        else:
            raw_label = "SINUS"
            color = "#00BFFF"
            confidence = s

        if raw_label == "AFIB" and afib_status == "SINUS":
            dom_html = f"<div style='font-size:36pt; color:yellow;'><b>SINUS** (Anomaly detected)</b></div>"
        elif raw_label == "SINUS" and afib_status == "SINUS":
            dom_html = f"<div style='font-size:36pt; color:{color};'><b>SINUS ({confidence*100:.0f}% confidence)</b></div>"
        else:
            dom_html = f"<div style='font-size:36pt; color:{color};'><b>AFIB ({confidence*100:.0f}% confidence)</b></div>"
    else:
        dom_html = "<div style='font-size:36pt; color:#666;'><b>----</b></div>"

    if hr is not None:
        hr_html = f"<div style='font-size:64pt; color:lime;'><b>HR:</b> {int(hr)}</div>"
    else:
        hr_html = "<div style='font-size:64pt; color:#666;'>HR: --</div>"

    text_label.setText("\n".join([
        dom_html,
        hr_html
    ]))

if __name__ == "__main__":
    init_csv_index()
    threading.Thread(target=serial_reader, daemon=True).start()
    app = setup_ui()
    timer = QtCore.QTimer()
    timer.timeout.connect(update_gui)
    timer.start(int(1000 / UPDATE_HZ))
    app.exec_()
