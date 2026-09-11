// DAB Classic – Gain-Satz eines Geraets (dab-api `Gain`).
// HackRF: LNA 0..40 (8er-Schritte), VGA 0..62 (2er-Schritte), AMP an/aus.
// RTL-SDR: lna = Tuner-Gain in 0,1 dB (naechster Wert aus
// rtlsdr_get_tuner_gains), vga/amp ohne Bedeutung.
#pragma once

struct DeviceGain {
    int  lna = 0;
    int  vga = 0;
    bool amp = false;
};
