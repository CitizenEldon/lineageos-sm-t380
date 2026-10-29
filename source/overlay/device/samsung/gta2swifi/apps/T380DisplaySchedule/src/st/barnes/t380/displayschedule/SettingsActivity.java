package st.barnes.t380.displayschedule;

import android.app.Activity;
import android.os.Bundle;
import android.text.InputType;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** Local-only configuration; no browser credentials or network capability. */
public final class SettingsActivity extends Activity {
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        ScheduleApplication app = (ScheduleApplication) getApplication();
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        layout.setPadding(pad, pad, pad, pad);
        TextView description = new TextView(this);
        description.setText("Choose local display hours. The screen stays visible between On and Off. "
                + "At night use the power button; normal Android idle timeout ends temporary use. "
                + "Other apps must not hold a screen-on lock at night. Confirm the local clock first.");
        layout.addView(description);
        EditText on = field(layout, "On (HH:mm)", app.preferences().getString("on", ""));
        EditText off = field(layout, "Off (HH:mm)", app.preferences().getString("off", ""));
        EditText idle = field(layout, "Night idle (minutes, 1–60)",
                app.preferences().contains("idle_minutes")
                        ? String.valueOf(app.preferences().getInt("idle_minutes", 0)) : "");
        idle.setInputType(InputType.TYPE_CLASS_NUMBER);
        TextView status = new TextView(this);
        status.setText(app.status());
        Button enable = new Button(this);
        enable.setText("Save and enable");
        enable.setOnClickListener(view -> {
            try {
                app.configure(on.getText().toString().trim(), off.getText().toString().trim(),
                        Integer.parseInt(idle.getText().toString().trim()));
                status.setText(app.status());
            } catch (RuntimeException error) {
                status.setText("Enter different On/Off times as HH:mm and idle minutes from 1 to 60.");
            }
        });
        layout.addView(enable);
        Button disable = new Button(this);
        disable.setText("Disable schedule");
        disable.setOnClickListener(view -> {
            try { app.disable(); status.setText(app.status()); }
            catch (RuntimeException error) { status.setText("Could not save disabled state"); }
        });
        layout.addView(disable);
        layout.addView(status);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(layout);
        setContentView(scroll);
    }

    private EditText field(LinearLayout layout, String hint, String value) {
        TextView label = new TextView(this);
        label.setText(hint);
        layout.addView(label);
        EditText field = new EditText(this);
        field.setSingleLine(true);
        field.setText(value);
        field.setInputType(InputType.TYPE_CLASS_TEXT);
        layout.addView(field);
        return field;
    }
}
