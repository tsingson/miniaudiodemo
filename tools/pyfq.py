
import scipy.signal as signal
import numpy as np

# Grid parameters matching the C setup
# Changed to odd number (191) to avoid Type II/IV filter restriction at Nyquist frequency
taps = 191  
fs_low = 1500.0  # Downsampled rate

# Normalize frequencies to Nyquist (fs_low / 2 = 750 Hz)
freqs = [0.0, 79.0, 81.0, 124.0, 126.0, 500.0, 550.0, 750.0]
norm_freqs = [f / (fs_low / 2.0) for f in freqs]

# Target gains mapped back from decibel values (+3dB -> ~1.41, +6dB -> ~2.0, +5dB -> ~1.78)
gains = [1.41, 2.0, 2.0, 1.78, 1.78, 1.41, 0.0, 0.0]

# Fixed function name: changed 'fir2' to 'firwin2'
coeffs = signal.firwin2(taps, norm_freqs, gains)

print(", ".join([f"{c:.6f}f" for c in coeffs]))



