package io.github.gajdi88.scpremote;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.graphics.Insets;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.provider.Settings;
import android.text.SpannableString;
import android.text.style.ForegroundColorSpan;
import android.text.style.RelativeSizeSpan;
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
    SeekBar volumeBar, subBar;              // portrait
    Fader volumeFader, subFader;            // landscape
    Button down, up, subDown, subUp;        // subDown / subUp: landscape only
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
        buildUi();
    }

    // The manifest keeps rotation from recreating the activity (configChanges), so
    // a download in progress and its listener survive it. The screen is simply
    // rebuilt for the new shape.
    @Override public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        buildUi();
        onStateChanged();
    }

    void buildUi() {
        draggingVolume = draggingSub = false;
        volumeBar = subBar = null;
        volumeFader = subFader = null;
        subDown = subUp = null;
        if (getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE) {
            buildLandscape();
        } else {
            buildPortrait();
        }
    }

    // ---- portrait: cards stacked, horizontal sliders --------------------------
    void buildPortrait() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        root.setPadding(pad, pad, pad, pad);

        nameView = text(15, TEXT);
        nameView.setText("MATCH UP 8DSP");
        nameView.setPadding(0, 0, 0, dp(12));
        root.addView(nameView);

        root.addView(fwWarning());

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
        in.addView(inputRow(56));
        TextView inHint = text(12, DIM);
        inHint.setText("The amp does not report the selected input, so this shows what this "
                + "app last sent, not necessarily the amp's state.");
        inHint.setPadding(0, dp(10), 0, 0);
        in.addView(inHint);

        // ---- amp tools
        LinearLayout tools = card(root);
        tools.addView(label("AMP TOOLS", 4));
        makeTools();
        tools.addView(startup, full());
        tools.addView(logButton, full());
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

        makeConnectionButtons();
        root.addView(startStop, full());
        root.addView(battery, full());
        root.addView(forget, full());

        versionView = text(11, DIM);
        versionView.setGravity(Gravity.CENTER);
        versionView.setPadding(0, dp(16), 0, 0);
        root.addView(versionView);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BG);
        scroll.addView(root);
        clearOfBars(scroll, root, pad);
        setContentView(scroll);
    }

    // ---- landscape: a mixing desk ----------------------------------------------
    // Two tall faders on the left, everything else in a scrolling panel on the right.
    // A horizontal slider across the whole width of a sideways phone is awkward;
    // a vertical fader under the thumb is not.
    void buildLandscape() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.HORIZONTAL);
        root.setBackgroundColor(BG);
        int pad = dp(12);
        root.setPadding(pad, pad, pad, pad);

        // ---- the two fader strips
        volumeFader = new Fader(this, LINE, ACCENT, TEXT);
        volumeView = text(40, TEXT);
        down = button("\u2212", v -> step(-1));
        up = button("+", v -> step(1));
        root.addView(strip("VOLUME", volumeFader, volumeView, up, down),
                column(1f, dp(10)));
        volumeFader.setListener(new Fader.Listener() {
            @Override public void onStart() { draggingVolume = true; }
            @Override public void onChange(int v) {
                if (AmpService.instance != null) AmpService.instance.setMaster(v);
                volumeView.setText(levelText(v, volumeFader));
            }
            @Override public void onStop(int v) {
                draggingVolume = false;
                AmpService s = AmpService.instance;
                if (s != null && v != s.shown()) s.setMaster(v);
            }
        });

        subFader = new Fader(this, LINE, ACCENT, TEXT);
        subView = text(32, TEXT);
        subDown = button("\u2212", v -> stepSub(-1));
        subUp = button("+", v -> stepSub(1));
        root.addView(strip("SUBWOOFER", subFader, subView, subUp, subDown),
                column(1f, dp(10)));
        subFader.setListener(new Fader.Listener() {
            @Override public void onStart() { draggingSub = true; }
            @Override public void onChange(int v) {
                if (AmpService.instance != null) AmpService.instance.setSub(v);
                subView.setText(levelText(v, subFader));
            }
            @Override public void onStop(int v) {
                draggingSub = false;
                AmpService s = AmpService.instance;
                if (s != null && v != s.subDesired) s.setSub(v);
            }
        });

        // ---- the panel: who, input, status, tools
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);

        LinearLayout head = new LinearLayout(this);
        head.setGravity(Gravity.CENTER_VERTICAL);
        nameView = text(15, TEXT);
        nameView.setText("MATCH UP 8DSP");
        head.addView(nameView, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        makeConnectionButtons();
        head.addView(startStop, new LinearLayout.LayoutParams(dp(110), dp(48)));
        panel.addView(head);

        panel.addView(fwWarning());

        statusView = text(13, DIM);
        statusView.setPadding(0, dp(2), 0, dp(10));
        panel.addView(statusView);

        LinearLayout in = card(panel);
        in.addView(label("INPUT", 10));
        in.addView(inputRow(52));

        LinearLayout tools = card(panel);
        tools.addView(label("AMP TOOLS", 4));
        makeTools();
        startup.setText("Re-run start-up");
        LinearLayout toolRow = new LinearLayout(this);
        LinearLayout.LayoutParams a = new LinearLayout.LayoutParams(0, dp(48), 1);
        LinearLayout.LayoutParams b = new LinearLayout.LayoutParams(0, dp(48), 1);
        b.leftMargin = dp(8);
        toolRow.addView(startup, a);
        toolRow.addView(logButton, b);
        tools.addView(toolRow);
        tools.addView(toolsNote);

        LinearLayout more = new LinearLayout(this);
        LinearLayout.LayoutParams c = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1);
        LinearLayout.LayoutParams d = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1);
        d.leftMargin = dp(8);
        battery.setText("Allow background");
        more.addView(battery, c);
        more.addView(forget, d);
        panel.addView(more);

        TextView hint = text(12, DIM);
        hint.setText("Phone volume buttons: main volume, also with the screen off.");
        hint.setPadding(0, dp(10), 0, 0);
        panel.addView(hint);

        versionView = text(11, DIM);
        versionView.setPadding(0, dp(8), 0, 0);
        panel.addView(versionView);

        ScrollView side = new ScrollView(this);
        side.addView(panel);
        root.addView(side, column(1.5f, 0));

        clearOfBars(root, root, pad);
        setContentView(root);
    }

    /** A tall card: label, then the fader beside a + / value / - column. */
    LinearLayout strip(String title, Fader fader, TextView value, Button plus, Button minus) {
        LinearLayout c = panelCard();
        c.addView(label(title, 6));
        LinearLayout body = new LinearLayout(this);
        body.addView(fader, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1));
        LinearLayout side = new LinearLayout(this);
        side.setOrientation(LinearLayout.VERTICAL);
        side.setGravity(Gravity.CENTER_HORIZONTAL);
        plus.setTextSize(TypedValue.COMPLEX_UNIT_SP, 24);
        minus.setTextSize(TypedValue.COMPLEX_UNIT_SP, 24);
        side.addView(plus, new LinearLayout.LayoutParams(dp(64), dp(64)));
        value.setGravity(Gravity.CENTER);
        value.setText("--");
        side.addView(value, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, 0, 1));
        side.addView(minus, new LinearLayout.LayoutParams(dp(64), dp(64)));
        body.addView(side, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.MATCH_PARENT));
        c.addView(body, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        return c;
    }

    /** "30" large, "/60" small, for the fader strips. */
    CharSequence levelText(int v, Fader f) {
        String n = String.valueOf(v), all = n + "\n/" + f.getMaxValue();
        SpannableString s = new SpannableString(all);
        s.setSpan(new RelativeSizeSpan(0.4f), n.length(), all.length(), 0);
        s.setSpan(new ForegroundColorSpan(DIM), n.length(), all.length(), 0);
        return s;
    }

    LinearLayout.LayoutParams column(float weight, int rightMargin) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.MATCH_PARENT, weight);
        p.rightMargin = rightMargin;
        return p;
    }

    // ---- pieces both layouts share -------------------------------------------
    // Shown only for firmware known to be unable to work, so it cannot be missed.
    TextView fwWarning() {
        fwWarning = text(13, 0xFFFF6B6B);
        fwWarning.setText("The ESP32 runs firmware v8, which cannot hear the amp's replies: the "
                + "levels never load and the controls stay disabled. Flash v8.1 or later "
                + "(scp_bench.ino).");
        fwWarning.setPadding(0, 0, 0, dp(12));
        fwWarning.setVisibility(View.GONE);
        return fwWarning;
    }

    LinearLayout inputRow(int height) {
        LinearLayout row = new LinearLayout(this);
        for (int i = 0; i < 3; i++) {
            final int idx = i;
            inputs[i] = button(INPUT_NAMES[i], v -> {
                if (AmpService.instance != null) AmpService.instance.setInput(idx);
            });
            LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0, dp(height), 1);
            if (i > 0) p.leftMargin = dp(8);
            row.addView(inputs[i], p);
        }
        return row;
    }

    void makeTools() {
        startup = button("Re-run start-up sequence", v -> {
            if (AmpService.instance != null) AmpService.instance.startupSequence();
        });
        logButton = button("Download log", v -> downloadLog());
        toolsNote = text(12, DIM);
        toolsNote.setPadding(0, dp(10), 0, 0);
    }

    void makeConnectionButtons() {
        startStop = button("Stop", v -> toggle());
        battery = button("Allow running in the background", v -> askBattery());
        forget = button("Forget this amp", v -> {
            if (AmpService.instance != null) AmpService.instance.forget();
        });
    }

    TextView label(String s, int below) {
        TextView t = text(13, DIM);
        t.setText(s);
        t.setPadding(0, 0, 0, dp(below));
        return t;
    }

    // Android 15 draws apps edge to edge, under the status and navigation bars (and,
    // sideways, the camera cutout), so keep the content clear of them. Older
    // versions lay out clear of the bars already.
    void clearOfBars(View host, View content, int pad) {
        if (Build.VERSION.SDK_INT < 35) return;
        host.setOnApplyWindowInsetsListener((v, insets) -> {
            Insets bars = insets.getInsets(WindowInsets.Type.systemBars()
                    | WindowInsets.Type.displayCutout());
            content.setPadding(pad + bars.left, pad + bars.top, pad + bars.right, pad + bars.bottom);
            return insets;
        });
        host.requestApplyInsets();
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

    void stepSub(int dir) {
        AmpService s = AmpService.instance;
        if (s != null && s.known) s.setSub(Math.max(0, s.subDesired) + dir);
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
        for (View v : new View[]{volumeBar, subBar, volumeFader, subFader, down, up, subDown, subUp,
                inputs[0], inputs[1], inputs[2]}) {
            if (v == null) continue;                // not in this orientation's layout
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
        int v = Math.max(0, s.shown());
        int sv = Math.max(0, s.subDesired);
        if (volumeBar != null) {
            volumeBar.setMax(s.max);
            subBar.setMax(s.maxSub);
            volumeView.setText(v + " / " + s.max);
            subView.setText(sv + " / " + s.maxSub);
            if (!draggingVolume) volumeBar.setProgress(v);
            if (!draggingSub) subBar.setProgress(sv);
        } else {
            volumeFader.setMax(s.max);
            subFader.setMax(s.maxSub);
            volumeFader.setValue(v);              // ignored while the user is dragging
            subFader.setValue(sv);
            if (!draggingVolume) volumeView.setText(levelText(v, volumeFader));
            if (!draggingSub) subView.setText(levelText(sv, subFader));
        }
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
        LinearLayout c = panelCard();
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(12);
        parent.addView(c, lp);
        return c;
    }

    /** The card itself, not yet placed anywhere. */
    LinearLayout panelCard() {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(PANEL);
        bg.setCornerRadius(dp(14));
        bg.setStroke(dp(1), LINE);
        c.setBackground(bg);
        int p = dp(16);
        c.setPadding(p, p, p, p);
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
