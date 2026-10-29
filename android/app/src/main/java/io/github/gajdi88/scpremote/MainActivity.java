package io.github.gajdi88.scpremote;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.ColorStateList;
import android.graphics.Insets;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * The same controls as the web app: master volume, subwoofer level and input.
 * The real work is in AmpService, which keeps running with the screen off and
 * also takes the phone's volume buttons for the master volume; opening this
 * screen starts it, as the web app connects on launch.
 */
public class MainActivity extends Activity implements AmpService.Listener {

    static final int BG = 0xFF111417, PANEL = 0xFF1A1F24, LINE = 0xFF2A3138, TEXT = 0xFFE8EDF2,
            DIM = 0xFF8B97A3, ACCENT = 0xFF4A9EFF;
    // Main, Optical, Extension: the colours the web app uses.
    static final int[] INPUT_COLORS = {0xFF3ECF7E, 0xFFF5C542, 0xFF4A9EFF};
    static final String[] INPUT_NAMES = {"Main", "Optical", "Extension"};

    TextView nameView, volumeView, subView, statusView, versionView, fwWarning;
    SeekBar volumeBar, subBar;
    Button down, up;
    final Button[] inputs = new Button[3];
    Button startStop, battery, forget;
    Button startup, logButton;
    TextView toolsNote;
    String toolsMessage;        // the last download's outcome, shown until the next one
    String pendingLog;          // downloaded, waiting for the user to save or share it
    static final int SAVE_LOG = 2;
    boolean draggingVolume, draggingSub;
    boolean asked;      // ask for permissions once per launch, not on every resume

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        root.setPadding(pad, pad, pad, pad);

        nameView = text(15, TEXT);
        nameView.setText("MATCH UP 8DSP");
        nameView.setPadding(0, 0, 0, dp(12));
        root.addView(nameView);

        // Shown only for firmware known to be unable to work, so it cannot be missed.
        fwWarning = text(13, 0xFFFF6B6B);
        fwWarning.setText("The ESP32 runs firmware v8, which cannot hear the amp's replies: the "
                + "levels never load and the controls stay disabled. Flash v8.1 or later "
                + "(scp_bench.ino).");
        fwWarning.setPadding(0, 0, 0, dp(12));
        fwWarning.setVisibility(View.GONE);
        root.addView(fwWarning);

        // ---- master volume
        LinearLayout vol = card(root);
        volumeView = valueRow(vol, "VOLUME", 44);
        volumeBar = seekBar(vol, new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar b, int v, boolean fromUser) {
                if (fromUser && AmpService.instance != null) AmpService.instance.setMaster(v);
            }
            @Override public void onStartTrackingTouch(SeekBar b) { draggingVolume = true; }
            @Override public void onStopTrackingTouch(SeekBar b) {
                draggingVolume = false;
                if (AmpService.instance != null) AmpService.instance.setMaster(b.getProgress());
            }
        });
        LinearLayout row = new LinearLayout(this);
        row.setPadding(0, dp(8), 0, 0);
        down = button("−", v -> step(-1));
        up = button("+", v -> step(1));
        down.setTextSize(TypedValue.COMPLEX_UNIT_SP, 24);
        up.setTextSize(TypedValue.COMPLEX_UNIT_SP, 24);
        row.addView(down, new LinearLayout.LayoutParams(0, dp(56), 1));
        row.addView(new View(this), new LinearLayout.LayoutParams(dp(10), 1));
        row.addView(up, new LinearLayout.LayoutParams(0, dp(56), 1));
        vol.addView(row);

        // ---- subwoofer
        LinearLayout sub = card(root);
        subView = valueRow(sub, "SUBWOOFER", 30);
        subBar = seekBar(sub, new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar b, int v, boolean fromUser) {
                if (fromUser && AmpService.instance != null) AmpService.instance.setSub(v);
            }
            @Override public void onStartTrackingTouch(SeekBar b) { draggingSub = true; }
            @Override public void onStopTrackingTouch(SeekBar b) {
                draggingSub = false;
                if (AmpService.instance != null) AmpService.instance.setSub(b.getProgress());
            }
        });

        // ---- input
        LinearLayout in = card(root);
        TextView inLabel = text(13, DIM);
        inLabel.setText("INPUT");
        inLabel.setPadding(0, 0, 0, dp(10));
        in.addView(inLabel);
        LinearLayout inRow = new LinearLayout(this);
        for (int i = 0; i < 3; i++) {
            final int idx = i;
            inputs[i] = button(INPUT_NAMES[i], v -> {
                if (AmpService.instance != null) AmpService.instance.setInput(idx);
            });
            LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0, dp(56), 1);
            if (i > 0) p.leftMargin = dp(8);
            inRow.addView(inputs[i], p);
        }
        in.addView(inRow);
        TextView inHint = text(12, DIM);
        inHint.setText("The amp does not report the selected input, so this shows what this "
                + "app last sent, not necessarily the amp's state.");
        inHint.setPadding(0, dp(10), 0, 0);
        in.addView(inHint);

        // ---- amp tools
        LinearLayout tools = card(root);
        TextView toolsLabel = text(13, DIM);
        toolsLabel.setText("AMP TOOLS");
        toolsLabel.setPadding(0, 0, 0, dp(4));
        tools.addView(toolsLabel);
        startup = button("Re-run start-up sequence", v -> {
            if (AmpService.instance != null) AmpService.instance.startupSequence();
        });
        tools.addView(startup, full());
        logButton = button("Download log", v -> downloadLog());
        tools.addView(logButton, full());
        toolsNote = text(12, DIM);
        toolsNote.setPadding(0, dp(10), 0, 0);
        tools.addView(toolsNote);

        // ---- connection
        statusView = text(13, DIM);
        statusView.setGravity(Gravity.CENTER);
        statusView.setPadding(0, dp(4), 0, dp(8));
        root.addView(statusView);

        TextView hint = text(12, DIM);
        hint.setText("While connected, the phone's volume buttons change the amp's main volume "
                + "(never the subwoofer), also with the screen off. Stop the connection to give "
                + "them back to the phone.");
        root.addView(hint);

        startStop = button("Stop", v -> toggle());
        root.addView(startStop, full());
        battery = button("Allow running in the background", v -> askBattery());
        root.addView(battery, full());
        forget = button("Forget this amp", v -> {
            if (AmpService.instance != null) AmpService.instance.forget();
        });
        root.addView(forget, full());

        versionView = text(11, DIM);
        versionView.setGravity(Gravity.CENTER);
        versionView.setPadding(0, dp(16), 0, 0);
        root.addView(versionView);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BG);
        scroll.addView(root);
        // Android 15 draws apps edge to edge, under the status and navigation bars,
        // so keep the content clear of them. Older versions lay out below the bars.
        if (Build.VERSION.SDK_INT >= 35) {
            scroll.setOnApplyWindowInsetsListener((v, insets) -> {
                Insets bars = insets.getInsets(WindowInsets.Type.systemBars()
                        | WindowInsets.Type.displayCutout());
                root.setPadding(pad + bars.left, pad + bars.top, pad + bars.right, pad + bars.bottom);
                return insets;
            });
        }
        setContentView(scroll);
    }

    @Override protected void onResume() {
        super.onResume();
        AmpService.listener = this;
        AmpService s = AmpService.instance;
        if (s != null) s.refresh();
        else if (!asked) startIfAllowed();
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

    // ---- event log ------------------------------------------------------------
    void downloadLog() {
        AmpService s = AmpService.instance;
        if (s == null) return;
        toolsMessage = null;
        toolsNote.setText("Downloading the log\u2026");
        s.downloadLog(new AmpService.LogListener() {
            @Override public void onLogProgress(int got, int total) {
                toolsNote.setText(total > 0
                        ? "Downloading the log\u2026 " + Math.min(100, got * 100 / total) + "%"
                        : "Downloading the log\u2026");
            }
            @Override public void onLogDone(String text) {
                pendingLog = header() + text;
                int lines = text.isEmpty() ? 0 : text.split("\n").length;
                toolsMessage = "Log downloaded: " + lines + " lines.";
                onStateChanged();
                if (isFinishing() || isDestroyed()) return;
                new AlertDialog.Builder(MainActivity.this)
                        .setTitle("Log downloaded")
                        .setMessage(lines + " lines from the ESP32. Save it as a file, or share it.")
                        .setPositiveButton("Save to file", (d, w) -> saveLog())
                        .setNeutralButton("Share", (d, w) -> shareLog())
                        .setNegativeButton("Cancel", null)
                        .show();
            }
            @Override public void onLogFailed(String why) {
                toolsMessage = why + ". Try again.";
                onStateChanged();
            }
        });
        onStateChanged();
    }

    String header() {
        AmpService s = AmpService.instance;
        String board = s != null && s.deviceName() != null ? s.deviceName() : "?";
        return "SCP event log from " + board + ", downloaded "
                + new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.US).format(new Date())
                + " by SCP Remote v" + versionName() + "\n"
                + "Lines: #<ESP32 boot> <time since that boot> [UTC time, once the phone set it]\n\n";
    }

    String fileName() {
        return "scp-log-" + new SimpleDateFormat("yyyy-MM-dd-HHmm", Locale.US).format(new Date()) + ".txt";
    }

    // The system "save as" screen: no storage permission needed, and the user
    // picks the place (Downloads, Drive, ...).
    void saveLog() {
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType("text/plain")
                .putExtra(Intent.EXTRA_TITLE, fileName());
        startActivityForResult(i, SAVE_LOG);
    }

    @Override protected void onActivityResult(int code, int result, Intent data) {
        super.onActivityResult(code, result, data);
        if (code != SAVE_LOG || result != RESULT_OK || data == null || data.getData() == null
                || pendingLog == null) return;
        try (OutputStream out = getContentResolver().openOutputStream(data.getData())) {
            out.write(pendingLog.getBytes(StandardCharsets.UTF_8));
            Toast.makeText(this, "Log saved", Toast.LENGTH_SHORT).show();
        } catch (Exception e) {
            Toast.makeText(this, "Could not save: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    void shareLog() {
        Intent i = new Intent(Intent.ACTION_SEND)
                .setType("text/plain")
                .putExtra(Intent.EXTRA_SUBJECT, fileName())
                .putExtra(Intent.EXTRA_TEXT, pendingLog);
        try {
            startActivity(Intent.createChooser(i, "Share the log"));
        } catch (RuntimeException e) {
            // A very large log can exceed what Android passes between apps.
            Toast.makeText(this, "Too large to share; use Save to file", Toast.LENGTH_LONG).show();
        }
    }

    // ---- display ------------------------------------------------------------
    @Override public void onStateChanged() {
        AmpService s = AmpService.instance;
        boolean running = s != null;
        boolean live = running && s.known;
        startStop.setText(running ? "Stop" : "Connect");
        battery.setVisibility(batteryExempt() ? View.GONE : View.VISIBLE);
        forget.setVisibility(running && s.deviceName() != null ? View.VISIBLE : View.GONE);
        for (View v : new View[]{volumeBar, subBar, down, up, inputs[0], inputs[1], inputs[2]}) {
            v.setEnabled(live);
            v.setAlpha(live ? 1f : 0.4f);
        }
        // The tools need only the Bluetooth link, not a level reading: the start-up
        // sequence is for exactly the case where the amp is not answering.
        boolean tooled = running && s.ready && s.supportsLog;
        boolean busy = running && (s.logPending || s.wantLog);
        startup.setEnabled(tooled && !busy);
        startup.setAlpha(tooled && !busy ? 1f : 0.4f);
        logButton.setEnabled(tooled && !busy);
        logButton.setAlpha(tooled && !busy ? 1f : 0.4f);
        if (!busy) {
            toolsNote.setText(running && s.ready && !s.supportsLog
                    ? "These need ESP32 firmware v8 or later."
                    : toolsMessage != null ? toolsMessage
                    : "The start-up sequence is what the CONDUCTOR knob sends when the amp "
                    + "switches on. The ESP32 sends it by itself; use this if the amp stays "
                    + "silent anyway.");
        }

        String name = running ? s.deviceName() : null;
        nameView.setText(name != null ? "MATCH UP 8DSP  ·  " + name : "MATCH UP 8DSP");
        statusView.setText(running ? s.status : "Not connected");
        String fw = running && s.ready ? (s.firmware != null ? s.firmware : "checking\u2026") : null;
        versionView.setText("App v" + versionName() + (fw != null ? "  \u00b7  ESP32 firmware " + fw : ""));
        fwWarning.setVisibility(running && s.firmwareBroken() ? View.VISIBLE : View.GONE);

        if (!live) {
            volumeView.setText("--");
            subView.setText("--");
            paintInputs(-1);
            return;
        }
        volumeBar.setMax(s.max);
        subBar.setMax(s.maxSub);
        int v = Math.max(0, s.shown());
        int sv = Math.max(0, s.subDesired);
        volumeView.setText(v + " / " + s.max);
        subView.setText(sv + " / " + s.maxSub);
        if (!draggingVolume) volumeBar.setProgress(v);
        if (!draggingSub) subBar.setProgress(sv);
        paintInputs(s.inputWanted);
    }

    void paintInputs(int selected) {
        for (int i = 0; i < 3; i++) {
            boolean on = i == selected;
            int c = INPUT_COLORS[i];
            GradientDrawable bg = new GradientDrawable();
            bg.setCornerRadius(dp(11));
            bg.setColor(on ? (c & 0x00FFFFFF) | 0x26000000 : 0x00000000);   // ~15% tint
            bg.setStroke(dp(1), on ? c : LINE);
            inputs[i].setBackground(bg);
            inputs[i].setBackgroundTintList(null);
            inputs[i].setTextColor(on ? c : DIM);
        }
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
        b.setBackgroundTintList(ColorStateList.valueOf(PANEL));
        b.setOnClickListener(l);
        return b;
    }

    /** A rounded panel like the web app's cards, added to the parent. */
    LinearLayout card(LinearLayout parent) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(PANEL);
        bg.setCornerRadius(dp(14));
        bg.setStroke(dp(1), LINE);
        c.setBackground(bg);
        int p = dp(16);
        c.setPadding(p, p, p, p);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(12);
        parent.addView(c, lp);
        return c;
    }

    /** Label on the left, large value on the right; returns the value view. */
    TextView valueRow(LinearLayout card, String label, int valueSp) {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.BOTTOM);
        TextView l = text(13, DIM);
        l.setText(label);
        row.addView(l, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        TextView v = text(valueSp, TEXT);
        v.setText("--");
        row.addView(v);
        card.addView(row);
        return v;
    }

    SeekBar seekBar(LinearLayout card, SeekBar.OnSeekBarChangeListener l) {
        SeekBar b = new SeekBar(this);
        b.setProgressTintList(ColorStateList.valueOf(ACCENT));
        b.setThumbTintList(ColorStateList.valueOf(ACCENT));
        b.setProgressBackgroundTintList(ColorStateList.valueOf(LINE));
        b.setOnSeekBarChangeListener(l);
        b.setPadding(dp(4), dp(14), dp(4), dp(14));
        card.addView(b, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        return b;
    }

    LinearLayout.LayoutParams full() {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        p.topMargin = dp(8);
        return p;
    }
}
