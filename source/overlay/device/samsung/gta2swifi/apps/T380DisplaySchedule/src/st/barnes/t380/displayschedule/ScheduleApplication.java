package st.barnes.t380.displayschedule;

import android.app.AlarmManager;
import android.app.Application;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.os.PowerManager;
import android.os.SystemClock;
import android.provider.Settings;
import android.util.Log;
import java.time.Instant;
import java.time.LocalTime;
import java.time.ZoneId;

public final class ScheduleApplication extends Application {
    private static final String TAG = "T380DisplaySchedule";
    private static final String BOUNDARY = "st.barnes.t380.displayschedule.BOUNDARY";
    private SharedPreferences prefs;
    private PowerManager power;
    private PowerManager.WakeLock visibleLock;
    private AlarmManager alarms;
    private PendingIntent boundary;
    private TimeoutOwnership timeoutOwnership;
    private boolean visible;
    private String status = "Disabled; hours have not been selected";

    @Override public void onCreate() {
        super.onCreate();
        prefs = createDeviceProtectedStorageContext().getSharedPreferences("schedule", MODE_PRIVATE);
        timeoutOwnership = new TimeoutOwnership(new TimeoutOwnership.Setting() {
            @Override public int read() { return timeout(); }
            @Override public void write(int value) {
                if (!Settings.System.putInt(getContentResolver(), Settings.System.SCREEN_OFF_TIMEOUT,
                        value)) throw new IllegalStateException("Could not write screen timeout");
            }
        }, new PreferenceTimeoutStore(new PreferenceTimeoutStore.Preferences() {
            private final SharedPreferences journal = createDeviceProtectedStorageContext()
                    .getSharedPreferences("timeout_ownership", MODE_PRIVATE);
            @Override public TimeoutOwnership.Record load() {
                if (!journal.contains("written_timeout")) return null;
                if (!journal.contains("original_timeout")) {
                    throw new IllegalStateException("Timeout baseline missing");
                }
                return new TimeoutOwnership.Record(journal.getInt("original_timeout", -1),
                        journal.getInt("written_timeout", -1));
            }
            @Override public boolean commit(TimeoutOwnership.Record record) {
                SharedPreferences.Editor editor = journal.edit();
                if (record == null) editor.remove("original_timeout").remove("written_timeout");
                else editor.putInt("original_timeout", record.original)
                        .putInt("written_timeout", record.written);
                return editor.commit();
            }
        }));
        power = getSystemService(PowerManager.class);
        alarms = getSystemService(AlarmManager.class);
        visibleLock = power.newWakeLock(PowerManager.SCREEN_BRIGHT_WAKE_LOCK, TAG + ":visible");
        visibleLock.setReferenceCounted(false);
        boundary = PendingIntent.getBroadcast(this, 0,
                new Intent(this, ScheduleReceiver.class).setAction(BOUNDARY),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        IntentFilter filter = new IntentFilter();
        filter.addAction(Intent.ACTION_SCREEN_ON);
        filter.addAction(Intent.ACTION_SCREEN_OFF);
        registerReceiver(new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) {
                // Power-button sleep disables screen wakelocks. Reacquire after
                // waking so the daytime display continues to stay visible.
                if (visible && Intent.ACTION_SCREEN_ON.equals(intent.getAction())) releaseLock();
                evaluate(false);
            }
        }, filter, Context.RECEIVER_NOT_EXPORTED);
        // A process restart at night must not cancel a physical-button override.
        // Boot/time/boundary/configuration receivers request enforced night-off.
        evaluate(false);
    }

    public String status() { return status; }
    public SharedPreferences preferences() { return prefs; }

    public void configure(String on, String off, int idleMinutes) {
        new DailySchedule(LocalTime.parse(on), LocalTime.parse(off));
        if (idleMinutes < 1 || idleMinutes > 60) {
            throw new IllegalArgumentException("Night idle must be 1–60 minutes");
        }
        SharedPreferences.Editor editor = prefs.edit().putString("on", on).putString("off", off)
                .putInt("idle_minutes", idleMinutes).putBoolean("enabled", true);
        if (!editor.commit()) throw new IllegalStateException("Could not persist configuration");
        evaluate(true);
    }

    public void disable() {
        if (!prefs.edit().putBoolean("enabled", false).commit()) {
            throw new IllegalStateException("Could not persist disabled state");
        }
        evaluate(false);
    }

    private int timeout() {
        return Settings.System.getInt(getContentResolver(), Settings.System.SCREEN_OFF_TIMEOUT, 60000);
    }
    private void releaseLock() {
        if (visibleLock.isHeld()) visibleLock.release();
    }
    public void evaluate(boolean enforceNightOff) {
        try {
            if (!prefs.getBoolean("enabled", false)) {
                visible = false;
                releaseLock();
                alarms.cancel(boundary);
                timeoutOwnership.restore();
                status = "Disabled";
                return;
            }
            if (!Settings.System.canWrite(this) || !alarms.canScheduleExactAlarms()) {
                throw new IllegalStateException("Missing settings or exact-alarm permission");
            }
            DailySchedule schedule = new DailySchedule(LocalTime.parse(prefs.getString("on", "")),
                    LocalTime.parse(prefs.getString("off", "")));
            DailySchedule.Decision decision = schedule.decide(Instant.now(), ZoneId.systemDefault());
            // Schedule first: do not leave the screen held on with no night boundary.
            alarms.setExactAndAllowWhileIdle(AlarmManager.RTC_WAKEUP,
                    decision.nextBoundary.toEpochMilli(), boundary);
            visible = decision.visible;
            if (visible) {
                timeoutOwnership.restore();
                if (!visibleLock.isHeld()) visibleLock.acquire();
                power.wakeUp(SystemClock.uptimeMillis(), PowerManager.WAKE_REASON_APPLICATION, TAG);
            } else {
                releaseLock();
                int nightTimeout = Math.multiplyExact(prefs.getInt("idle_minutes", 0), 60000);
                if (nightTimeout < 60000 || nightTimeout > 3600000) {
                    throw new IllegalStateException("Invalid persisted idle timeout");
                }
                timeoutOwnership.apply(nightTimeout);
                if (enforceNightOff) power.goToSleep(SystemClock.uptimeMillis(),
                        PowerManager.GO_TO_SLEEP_REASON_APPLICATION, 0);
            }
            status = (visible ? "Daytime display on" : "Nighttime; power button allows temporary use")
                    + "; next boundary " + decision.nextBoundary.atZone(ZoneId.systemDefault());
        } catch (RuntimeException error) {
            visible = false;
            FailureCleanup.Result cleanup = FailureCleanup.run(this::releaseLock,
                    () -> alarms.cancel(boundary), timeoutOwnership);
            status = "Schedule unavailable: " + error.getClass().getSimpleName()
                    + (cleanup.timeoutRecoveryPending ? "; timeout recovery pending" : "")
                    + cleanup.failures;
            Log.e(TAG, status, error);
        }
    }
}
