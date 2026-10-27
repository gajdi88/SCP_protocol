package io.github.gajdi88.scpremote;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/**
 * One screen: the amp's master volume, -/+ buttons, the link status and the
 * version. The real work is in AmpService, which keeps running with the screen
 * off; opening this screen starts it, as the web app connects on launch.
 */
public class MainActivity extends Activity implements AmpService.Listener {

    static final int BG = 0xFF111417, PANEL = 0xFF1A1F24, TEXT = 0xFFE8EDF2, DIM = 0xFF8B97A3;

    TextView volumeView, statusView, nameView;
    Button startStop, battery, forget;
    boolean asked;      // ask for permissions once per launch, not on every resume

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(BG);
        int pad = dp(20);
        root.setPadding(pad, pad, pad, pad);

        nameView = text(15, TEXT);
        nameView.setText("MATCH UP 8DSP");
        root.addView(nameView);

        TextView label = text(13, DIM);
        label.setText("VOLUME");
        label.setPadding(0, dp(28), 0, 0);
        root.addView(label);

        volumeView = text(64, TEXT);
        volumeView.setText("--");
        root.addView(volumeView);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        Button down = button("−", v -> step(-1));
        Button up = button("+", v -> step(1));
        down.setTextSize(TypedValue.COMPLEX_UNIT_SP, 28);
        up.setTextSize(TypedValue.COMPLEX_UNIT_SP, 28);
        row.addView(down, new LinearLayout.LayoutParams(0, dp(72), 1));
        View gap = new View(this);
        row.addView(gap, new LinearLayout.LayoutParams(dp(12), 1));
        row.addView(up, new LinearLayout.LayoutParams(0, dp(72), 1));
        root.addView(row);

        TextView hint = text(13, DIM);
        hint.setText("While connected, the phone's volume buttons change the amp's main volume, "
                + "also with the screen off. Stop the connection to give them back to the phone.");
        hint.setPadding(0, dp(16), 0, dp(16));
        root.addView(hint);

        statusView = text(14, DIM);
        root.addView(statusView);

        startStop = button("Stop", v -> toggle());
        root.addView(startStop, full());
        battery = button("Allow running in the background", v -> askBattery());
        root.addView(battery, full());
        forget = button("Forget this amp", v -> {
            if (AmpService.instance != null) AmpService.instance.forget();
        });
        root.addView(forget, full());

        View spring = new View(this);
        root.addView(spring, new LinearLayout.LayoutParams(1, 0, 1));

        TextView version = text(11, DIM);
        version.setGravity(Gravity.CENTER);
        version.setText("v" + versionName());
        root.addView(version);

        setContentView(root);
    }

    @Override protected void onResume() {
        super.onResume();
        AmpService.listener = this;
        if (AmpService.instance == null && !asked) startIfAllowed();
        else AmpService.instance.refresh();
        onStateChanged();
    }

    @Override protected void onPause() {
        AmpService.listener = null;
        super.onPause();
    }

    // ---- service ------------------------------------------------------------
    void step(int dir) {
        if (AmpService.instance != null) AmpService.instance.step(dir);
    }

    void toggle() {
        if (AmpService.instance != null) {
            stopService(new Intent(this, AmpService.class));
        } else {
            startIfAllowed();
        }
        onStateChanged();
    }

    void startIfAllowed() {
        List<String> missing = missingPermissions();
        if (!missing.isEmpty()) {
            asked = true;
            requestPermissions(missing.toArray(new String[0]), 1);
            return;
        }
        startForegroundService(new Intent(this, AmpService.class));
    }

    @Override public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        // Notifications are optional; without them the service still runs, just
        // without a visible status line.
        List<String> missing = missingPermissions();
        missing.remove(Manifest.permission.POST_NOTIFICATIONS);
        if (missing.isEmpty()) {
            startForegroundService(new Intent(this, AmpService.class));
        } else {
            statusView.setText("Bluetooth permission is needed to reach the amp.");
        }
    }

    List<String> missingPermissions() {
        List<String> want = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= 31) {
            want.add(Manifest.permission.BLUETOOTH_SCAN);
            want.add(Manifest.permission.BLUETOOTH_CONNECT);
        } else {
            want.add(Manifest.permission.ACCESS_FINE_LOCATION);
        }
        if (Build.VERSION.SDK_INT >= 33) want.add(Manifest.permission.POST_NOTIFICATIONS);
        List<String> missing = new ArrayList<>();
        for (String p : want) {
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) missing.add(p);
        }
        return missing;
    }

    // Some phones kill background work after a while with the screen off unless
    // the app is exempt from battery optimisation.
    @SuppressLint("BatteryLife")
    void askBattery() {
        Intent i = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                Uri.parse("package:" + getPackageName()));
        startActivity(i);
    }

    boolean batteryExempt() {
        PowerManager pm = getSystemService(PowerManager.class);
        return pm != null && pm.isIgnoringBatteryOptimizations(getPackageName());
    }

    // ---- display ------------------------------------------------------------
    @Override public void onStateChanged() {
        AmpService s = AmpService.instance;
        boolean running = s != null;
        startStop.setText(running ? "Stop" : "Connect");
        battery.setVisibility(batteryExempt() ? View.GONE : View.VISIBLE);
        forget.setVisibility(running && s.deviceName() != null ? View.VISIBLE : View.GONE);
        if (!running) {
            volumeView.setText("--");
            statusView.setText("Not connected");
            nameView.setText("MATCH UP 8DSP");
            return;
        }
        nameView.setText(s.deviceName() != null ? "MATCH UP 8DSP  ·  " + s.deviceName() : "MATCH UP 8DSP");
        volumeView.setText(s.known && s.shown() >= 0 ? s.shown() + " / " + s.max : "--");
        statusView.setText(s.status);
    }

    String versionName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException e) {
            return "?";
        }
    }

    // ---- view helpers -------------------------------------------------------
    int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    TextView text(int sp, int color) {
        TextView t = new TextView(this);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        return t;
    }

    Button button(String label, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextColor(TEXT);
        b.setBackgroundTintList(android.content.res.ColorStateList.valueOf(PANEL));
        b.setOnClickListener(l);
        return b;
    }

    LinearLayout.LayoutParams full() {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        p.topMargin = dp(8);
        return p;
    }
}
