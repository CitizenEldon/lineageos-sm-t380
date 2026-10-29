/* Scoped SM-T380 measured telemetry, independent of charging admission. */
package com.android.server;

final class T380Telemetry {
    static final String PATH = "/sys/devices/platform/i2c-fuelgauge/i2c-7/7-0071/android_telemetry";
    static final String UNAVAILABLE_EXTRA = "t380.telemetry_unavailable";
    static final long FRESH_MS = 65000;
    static final class Sample {
        long sequence, sampledMs, temperatureMs;
        boolean valid, temperatureValid;
        int cause, error, capacity, temperature, status;
        static Sample parse(String text, long bootMs) {
            try {
                String[] words = text.trim().split("\\s+");
                String[] keys = {"t380_measured_v1", "seq", "sampled_ms", "valid", "cause",
                    "error", "capacity", "voltage_uv", "current_ua", "temp_valid", "temp",
                    "temp_ms", "status"};
                if (words.length != keys.length) return null;
                long[] v = new long[keys.length];
                for (int i = 0; i < keys.length; i++) {
                    String[] pair = words[i].split("=", -1);
                    if (pair.length != 2 || !pair[0].equals(keys[i])) return null;
                    v[i] = Long.parseLong(pair[1]);
                }
                if (v[0] != 1 || v[1] < 0 || v[2] < 0 || v[3] < 0 || v[3] > 1 ||
                        v[4] < 0 || v[4] > 4 || v[5] > 0 || v[9] < 0 || v[9] > 1 ||
                        v[11] < 0 || v[12] < 0 || v[12] > 4 ||
                        (v[3] == 1 && (v[1] == 0 || v[4] != 0 || v[5] != 0 ||
                            v[6] < 0 || v[6] > 100))) return null;
                for (int i : new int[] {4, 5, 6, 7, 8, 10, 12}) {
                    if (v[i] < Integer.MIN_VALUE || v[i] > Integer.MAX_VALUE) return null;
                }
                Sample s = new Sample();
                s.sequence = v[1]; s.sampledMs = v[2]; s.cause = (int)v[4];
                s.error = (int)v[5]; s.capacity = (int)v[6]; s.temperature = (int)v[10];
                s.temperatureMs = v[11]; s.status = (int)v[12] + 1;
                s.valid = v[3] == 1 && bootMs >= s.sampledMs && bootMs-s.sampledMs <= FRESH_MS;
                s.temperatureValid = v[9] == 1 && bootMs >= s.temperatureMs &&
                        bootMs-s.temperatureMs <= FRESH_MS;
                return s;
            } catch (NumberFormatException e) { return null; }
        }
    }

    boolean ready;
    int level;

    void observe(Sample s, long uptime) {
        // Transient telemetry loss is reported, not latched until reboot.
        // The kernel controls charging and handles physical faults autonomously.
        ready = s != null && s.valid && s.error == 0;
        if (ready) level = s.capacity;
    }
}
