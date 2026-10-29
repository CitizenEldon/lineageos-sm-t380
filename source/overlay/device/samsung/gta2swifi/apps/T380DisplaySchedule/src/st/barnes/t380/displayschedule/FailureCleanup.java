package st.barnes.t380.displayschedule;

/** The actual failure path, independently best-effort across all cleanup steps. */
public final class FailureCleanup {
    public static final class Result {
        public final boolean timeoutRecoveryPending;
        public final String failures;
        Result(boolean timeoutRecoveryPending, String failures) {
            this.timeoutRecoveryPending = timeoutRecoveryPending;
            this.failures = failures;
        }
    }

    public static Result run(Runnable releaseLock, Runnable cancelAlarm, TimeoutOwnership timeout) {
        String failures = attempt("screen lock", releaseLock)
                + attempt("alarm", cancelAlarm)
                + attempt("timeout", timeout::restore);
        boolean pending;
        try { pending = timeout.pending(); }
        catch (RuntimeException error) {
            pending = true;
            failures += " timeout ownership: " + error.getClass().getSimpleName() + ";";
        }
        return new Result(pending, failures);
    }

    private static String attempt(String label, Runnable action) {
        try { action.run(); return ""; }
        catch (RuntimeException error) {
            return " " + label + ": " + error.getClass().getSimpleName() + ";";
        }
    }
}
