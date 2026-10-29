package st.barnes.t380.displayschedule;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

public final class ScheduleReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        ((ScheduleApplication) context.getApplicationContext()).evaluate(true);
    }
}
