package st.barnes.t380.displayschedule;

/** Owns only timeout values this app journaled before writing to Android. */
public final class TimeoutOwnership {
    public interface Setting {
        int read();
        void write(int timeout);
    }
    public interface Store {
        Record read();
        void save(Record record);
        void clear();
    }
    public static final class Record {
        public final int original;
        public final int written;
        public Record(int original, int written) {
            this.original = original;
            this.written = written;
        }
    }

    private final Setting setting;
    private final Store store;

    public TimeoutOwnership(Setting setting, Store store) {
        this.setting = setting;
        this.store = store;
    }

    public boolean pending() { return store.read() != null; }

    public void apply(int target) {
        Record record = store.read();
        if (record != null && record.written != target) {
            // Never replace the old applied/pending marker with a new target.
            // A failed restore leaves the old baseline and marker recoverable.
            restore();
            record = null;
        }
        int current = setting.read();
        if (record != null && current != record.written && current != record.original) {
            // Resolve an unrelated edit without writing over it during cleanup.
            store.clear();
            record = null;
        }
        if (record == null) {
            record = new Record(current, target);
            // Durable journal must succeed before actuation. A crash after this
            // point is recoverable whether the setting write ran or not.
            store.save(record);
        }
        if (current != target) setting.write(target);
    }

    public void restore() {
        Record record = store.read();
        if (record == null) return;
        if (setting.read() == record.written) setting.write(record.original);
        // If the current setting differs, preserve the unrelated edit (or the
        // original baseline after a crash between restoration and clear).
        // Retain the record if either write or durable clear fails.
        store.clear();
    }
}
