package st.barnes.t380.displayschedule;

/** Dedicated journal, never edited by runtime schedule configuration. */
final class PreferenceTimeoutStore implements TimeoutOwnership.Store {
    interface Preferences {
        TimeoutOwnership.Record load();
        boolean commit(TimeoutOwnership.Record record);
    }
    private final Preferences prefs;
    private TimeoutOwnership.Record record;

    PreferenceTimeoutStore(Preferences prefs) {
        this.prefs = prefs;
        record = prefs.load();
    }

    @Override public TimeoutOwnership.Record read() { return record; }

    @Override public void save(TimeoutOwnership.Record next) {
        if (!prefs.commit(next)) {
            throw new IllegalStateException("Could not persist timeout ownership");
        }
        record = next;
    }

    @Override public void clear() {
        if (!prefs.commit(null)) {
            // SharedPreferences can change its in-memory map before commit fails.
            // Preserve our recovery record until durable clearing succeeds.
            throw new IllegalStateException("Could not clear timeout ownership");
        }
        record = null;
    }
}
