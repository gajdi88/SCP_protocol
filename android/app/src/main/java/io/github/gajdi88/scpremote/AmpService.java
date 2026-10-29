package io.github.gajdi88.scpremote;

import android.annotation.SuppressLint;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ServiceInfo;
import android.graphics.drawable.Icon;
import android.media.VolumeProvider;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;

import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Holds the Bluetooth link to the amp while the screen is off, and claims the
 * phone's volume buttons for the amp's master volume.
 *
 * The buttons are claimed the way Cast apps claim them: a media session whose
 * playback is "remote", with a VolumeProvider standing in for the speaker. While
 * that session is active and playing, Android sends volume key presses to it
 * instead of changing the phone's own volume, including on the lock screen.
 *
 * It speaks the same text console as the web app, over the Nordic UART Service.
 * Everything runs on the main thread: Bluetooth callbacks arrive on binder
 * threads and are posted across before touching any state.
 */
@SuppressLint("MissingPermission")   // MainActivity starts this only once permissions are granted
public class AmpService extends Service {

    static final UUID NUS = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
    static final UUID RX = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e");   // we write here
    static final UUID TX = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e");   // amp notifies here
    static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    static final String ACTION_STOP = "io.github.gajdi88.scpremote.STOP";
    static final String PREFS = "amp", KEY_ADDR = "address", KEY_NAME = "name";
    static final String CHANNEL = "link";
    static final int NOTE_ID = 1;

    static final long STALE_MS = 15000;     // re-read the amp before stepping from an older value
    static final long REPLY_MS = 2500;      // give up waiting for OK after this
    static final long SCAN_MS = 15000;

    static final Pattern CEILING = Pattern.compile("master\\s+0\\.\\.(\\d+),\\s*sub\\s+0\\.\\.(\\d+)");
    static final Pattern LEVELS = Pattern.compile("master=(\\d+).*sub=(\\d+)");

    /** The activity listens here; both live on the main thread of one process. */
    interface Listener { void onStateChanged(); }
    static AmpService instance;
    static Listener listener;

    final Handler main = new Handler(Looper.getMainLooper());
    SharedPreferences prefs;
    BluetoothAdapter adapter;
    BluetoothGatt gatt;
    BluetoothGattCharacteristic rx;
    MediaSession session;
    VolumeProvider volume;

    String status = "Starting…";
    boolean ready;          // GATT up and notifications on
    boolean known;          // the amp's level has been read since connecting
    boolean stopped;
    int max = 60;           // replaced by the firmware's own ceiling from '?'
    int confirmed = -1;     // last level the amp reported or accepted
    int desired = -1;       // level the user is asking for
    int pendingSteps;       // presses made while a fresh read is outstanding
    int sentValue;
    long lastSync;

    // The sub and the input are set from the screen only, never by the volume keys.
    int maxSub = 23;
    int subConfirmed = -1, subDesired = -1, subSent;
    // The amp does not report its input, so this is what the app last asked for.
    int inputWanted = -1, inputDone = -1, inputSent;

    // Firmware v8 and later: start-up sequence ('h'), event log ('log'), clock ('t').
    // Recognised from its help line, so older firmware keeps working without them.
    boolean supportsLog;
    boolean clockSent;

    // The ESP32 firmware version: "v8.2" etc., or null until known. v8.2 and later
    // say it in their help; older ones are recognised by what they print (fwProbe).
    String firmware;

    /** v8 pulled the RX pin down and cannot hear the amp's replies at all. */
    boolean firmwareBroken() { return "v8".equals(firmware); }
    boolean wantHandshake, wantLog, logPending;
    StringBuilder logText;
    int logTotal;
    LogListener logListener;

    /** Progress and result of a log download, on the main thread. */
    interface LogListener {
        void onLogProgress(int got, int total);
        void onLogDone(String text);
        void onLogFailed(String why);
    }
    int retry;
    boolean scanning;

    // Web Bluetooth and Android both reject overlapping GATT operations, so writes
    // go out one at a time. Commands answered by OK/FAILED are tracked in 'owed', and
    // no new level is sent until the last one is answered: that coalesces fast key
    // presses into few writes and keeps the ESP32's 8-deep command queue from
    // overflowing.
    final ArrayDeque<byte[]> writes = new ArrayDeque<>();
    boolean writing;
    final ArrayDeque<Character> owed = new ArrayDeque<>();
    boolean needRead;
    final StringBuilder lineBuf = new StringBuilder();

    // ---- lifecycle ------------------------------------------------------------
    @Override public void onCreate() {
        super.onCreate();
        instance = this;
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        BluetoothManager bm = getSystemService(BluetoothManager.class);
        adapter = bm == null ? null : bm.getAdapter();

        NotificationChannel ch = new NotificationChannel(CHANNEL, "Amp connection",
                NotificationManager.IMPORTANCE_LOW);
        getSystemService(NotificationManager.class).createNotificationChannel(ch);
        try {
            if (Build.VERSION.SDK_INT >= 29) {
                startForeground(NOTE_ID, buildNotification(),
                        ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
            } else {
                startForeground(NOTE_ID, buildNotification());
            }
        } catch (RuntimeException e) {
            // Android 12+ refuses a foreground start from the background, e.g. a
            // sticky restart. The user reopening the app starts it properly.
            stopped = true;
            stopSelf();
            return;
        }

        session = new MediaSession(this, "SCP");
        // Volume key presses reach the VolumeProvider through the session's callback
        // handler: with no callback set, MediaSession drops them silently, and the
        // system still shows its volume panel as if they had worked.
        session.setCallback(new MediaSession.Callback() {}, main);
        session.setPlaybackState(new PlaybackState.Builder()
                .setState(PlaybackState.STATE_PLAYING, 0, 0f).build());

        startLink();
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            stopSelf();
            return START_NOT_STICKY;
        }
        return START_STICKY;
    }

    @Override public void onDestroy() {
        stopped = true;
        main.removeCallbacksAndMessages(null);
        stopScan();
        if (gatt != null) { gatt.disconnect(); gatt.close(); gatt = null; }
        if (session != null) session.release();
        instance = null;
        notifyUi();
        super.onDestroy();
    }

    @Override public IBinder onBind(Intent intent) { return null; }

    // ---- public to the activity ---------------------------------------------
    /** Step the master volume, as a volume key press would. */
    void step(int dir) {
        if (!known || dir == 0) return;
        if (System.currentTimeMillis() - lastSync > STALE_MS) {
            // The knob may have been turned since: read first, apply the steps after.
            pendingSteps += dir;
            if (!owed.contains('r')) needRead = true;
        } else {
            desired = clamp((desired >= 0 ? desired : confirmed) + dir);
        }
        showVolume();
        pump();
    }

    /** Set the master volume directly, from the slider or the system volume panel. */
    void setMaster(int v) {
        if (!known) return;
        desired = clamp(v);
        pendingSteps = 0;
        showVolume();
        pump();
    }

    void setSub(int v) {
        if (!known) return;
        subDesired = Math.max(0, Math.min(maxSub, v));
        notifyUi();
        pump();
    }

    void setInput(int i) {
        if (!known || i < 0 || i > 2) return;
        inputWanted = i;
        notifyUi();
        pump();
    }

    /**
     * Ask the ESP32 to replay the knob's power-up sequence. It does this by itself
     * when the amp comes up and when a write is refused; this is for the case where
     * neither happened and the amp still is not answering. Allowed while the levels
     * are unknown, which is exactly when it is needed.
     */
    void startupSequence() {
        if (!ready || !supportsLog) return;
        wantHandshake = true;
        setStatus("Sending the start-up sequence\u2026");
        pump();
    }

    /** Fetch the ESP32's event log; the listener gets progress and the text. */
    void downloadLog(LogListener l) {
        if (!ready || !supportsLog || logPending || wantLog) return;
        logListener = l;
        wantLog = true;
        pump();
    }

    /** Re-read the level, e.g. when the app comes to the front. */
    void refresh() {
        if (ready && !owed.contains('r')) { needRead = true; pump(); }
    }

    /** Drop the remembered amp and search again, for when the board is replaced. */
    void forget() {
        prefs.edit().clear().apply();
        dropLink();
        retry = 0;
        startLink();
    }

    String deviceName() { return prefs.getString(KEY_NAME, null); }
    int shown() { return desired >= 0 ? clamp(desired + pendingSteps) : -1; }

    // ---- finding and connecting ---------------------------------------------
    final Runnable retryRunnable = this::startLink;
    final Runnable scanTimeout = () -> {
        stopScan();
        setStatus("Amp not found, still looking…");
        retryLater();
    };

    void startLink() {
        if (stopped || gatt != null || scanning) return;
        if (adapter == null || !adapter.isEnabled()) {
            setStatus("Bluetooth is off");
            retryLater();
            return;
        }
        BluetoothLeScanner scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) { retryLater(); return; }

        // Android pauses unfiltered scans while the screen is off, so once the amp
        // is known, scan for its address. The very first search happens with the
        // app open, where an unfiltered scan for the SCP- name is fine.
        String addr = prefs.getString(KEY_ADDR, null);
        List<ScanFilter> filters = new ArrayList<>();
        if (addr != null) filters.add(new ScanFilter.Builder().setDeviceAddress(addr).build());
        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();

        setStatus(addr == null ? "Searching for the amp…" : "Looking for " + deviceName() + "…");
        scanning = true;
        scanner.startScan(filters, settings, scanCallback);
        main.postDelayed(scanTimeout, SCAN_MS);
    }

    void stopScan() {
        if (!scanning) return;
        scanning = false;
        main.removeCallbacks(scanTimeout);
        try {
            BluetoothLeScanner s = adapter.getBluetoothLeScanner();
            if (s != null) s.stopScan(scanCallback);
        } catch (RuntimeException ignored) { /* adapter switched off meanwhile */ }
    }

    final ScanCallback scanCallback = new ScanCallback() {
        @Override public void onScanResult(int type, ScanResult result) {
            main.post(() -> onFound(result));
        }
        @Override public void onScanFailed(int code) {
            main.post(() -> {
                stopScan();
                setStatus("Bluetooth scan failed (" + code + "), retrying…");
                retryLater();
            });
        }
    };

    void onFound(ScanResult result) {
        if (!scanning) return;
        BluetoothDevice dev = result.getDevice();
        String name = result.getScanRecord() != null ? result.getScanRecord().getDeviceName() : null;
        if (name == null) name = dev.getName();
        if (prefs.getString(KEY_ADDR, null) == null) {
            if (name == null || !name.startsWith("SCP-")) return;
        }
        stopScan();
        SharedPreferences.Editor e = prefs.edit().putString(KEY_ADDR, dev.getAddress());
        if (name != null) e.putString(KEY_NAME, name);
        e.apply();

        setStatus("Connecting to " + deviceName() + "…");
        gatt = dev.connectGatt(this, false, gattCallback, BluetoothDevice.TRANSPORT_LE);
    }

    /** Back off from 2 s to 30 s, so a switched-off amp does not drain the battery. */
    void retryLater() {
        if (stopped) return;
        long delay = Math.min(2000L << Math.min(retry, 4), 30000L);
        retry++;
        main.removeCallbacks(retryRunnable);
        main.postDelayed(retryRunnable, delay);
    }

    void dropLink() {
        stopScan();
        ready = false;
        known = false;
        rx = null;
        writes.clear();
        writing = false;
        owed.clear();
        needRead = false;
        pendingSteps = 0;
        lineBuf.setLength(0);
        main.removeCallbacks(replyTimeout);
        main.removeCallbacks(retryRead);
        main.removeCallbacks(fwProbe);
        if (logPending) finishLog(null, "The connection dropped during the download");
        wantHandshake = false;
        wantLog = false;
        if (gatt != null) { gatt.disconnect(); gatt.close(); gatt = null; }
        // Hand the volume buttons back to the phone while there is no amp to drive.
        if (session != null) session.setActive(false);
    }

    void linkLost(String why) {
        dropLink();
        setStatus(why);
        retryLater();
    }

    // ---- GATT -------------------------------------------------------------
    final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {
        @Override public void onConnectionStateChange(BluetoothGatt g, int st, int state) {
            main.post(() -> {
                if (g != gatt) return;
                if (state == BluetoothProfile.STATE_CONNECTED && st == BluetoothGatt.GATT_SUCCESS) {
                    setStatus("Connected, setting up…");
                    g.discoverServices();
                } else {
                    linkLost(ready ? "Link lost, reconnecting…" : "Could not connect, retrying…");
                }
            });
        }

        @SuppressWarnings("deprecation")   // the pre-Android 13 descriptor write
        @Override public void onServicesDiscovered(BluetoothGatt g, int st) {
            main.post(() -> {
                if (g != gatt) return;
                BluetoothGattService svc = g.getService(NUS);
                BluetoothGattCharacteristic tx = svc == null ? null : svc.getCharacteristic(TX);
                rx = svc == null ? null : svc.getCharacteristic(RX);
                BluetoothGattDescriptor cccd = tx == null ? null : tx.getDescriptor(CCCD);
                if (rx == null || cccd == null) {
                    linkLost("That device is not an SCP amp bridge");
                    return;
                }
                g.setCharacteristicNotification(tx, true);
                byte[] on = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE;
                writing = true;
                if (Build.VERSION.SDK_INT >= 33) {
                    g.writeDescriptor(cccd, on);
                } else {
                    cccd.setValue(on);
                    g.writeDescriptor(cccd);
                }
            });
        }

        @Override public void onDescriptorWrite(BluetoothGatt g, BluetoothGattDescriptor d, int st) {
            main.post(() -> {
                if (g != gatt) return;
                writing = false;
                onLinkReady();
            });
        }

        @Override public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int st) {
            main.post(() -> {
                if (g != gatt) return;
                writing = false;
                nextWrite();
            });
        }

        @Override public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c,
                                                      byte[] value) {
            byte[] copy = value.clone();
            main.post(() -> { if (g == gatt) onBytes(copy); });
        }

        @SuppressWarnings("deprecation")
        @Override public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
            if (Build.VERSION.SDK_INT >= 33) return;     // the overload above handles it
            byte[] copy = c.getValue().clone();
            main.post(() -> { if (g == gatt) onBytes(copy); });
        }
    };

    void onLinkReady() {
        ready = true;
        retry = 0;
        clockSent = false;
        firmware = null;                    // the board may have been reflashed since
        setStatus("Reading the amp…");
        send("v 0", 'v');   // quiet mode: one line per command instead of four
        sendRaw("?");       // learn this firmware's ceiling; answered without OK
        needRead = true;
        pump();
    }

    void sendRaw(String cmd) {
        writes.add((cmd + "\n").getBytes(StandardCharsets.US_ASCII));
        nextWrite();
    }

    void send(String cmd, char kind) {
        owed.add(kind);
        main.removeCallbacks(replyTimeout);
        main.postDelayed(replyTimeout, REPLY_MS);
        sendRaw(cmd);
    }

    @SuppressWarnings("deprecation")
    void nextWrite() {
        if (writing || rx == null || gatt == null || writes.isEmpty()) return;
        byte[] data = writes.poll();
        writing = true;
        boolean started;
        if (Build.VERSION.SDK_INT >= 33) {
            started = gatt.writeCharacteristic(rx, data,
                    BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothGatt.GATT_SUCCESS;
        } else {
            rx.setValue(data);
            rx.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            started = gatt.writeCharacteristic(rx);
        }
        if (!started) linkLost("Write failed, reconnecting…");
    }

    // ---- the console protocol ---------------------------------------------
    final Runnable replyTimeout = () -> {
        owed.clear();
        pendingSteps = 0;
        if (desired != confirmed) desired = confirmed;
        subDesired = subConfirmed;
        inputWanted = inputDone;
        setStatus("No answer from the amp — is it powered?");
        showVolume();
        // Until the first level is read the buttons are not claimed, so keep asking.
        if (!known) main.postDelayed(this.retryRead, 3000);
    };

    final Runnable retryRead = () -> { needRead = true; pump(); };

    // 'd' answers with plain lines and no OK, so it goes out untracked.
    final Runnable fwProbe = () -> { if (ready && firmware == null && !logPending) sendRaw("d"); };

    /** Send whatever is next: a read if one is due, else the newest wanted settings. */
    void pump() {
        // While the ESP32 streams its log it runs nothing else, so hold everything.
        if (!ready || !owed.isEmpty() || logPending) return;
        if (wantHandshake) {
            wantHandshake = false;
            send("h", 'h');
        } else if (wantLog) {
            wantLog = false;
            logPending = true;
            logText = new StringBuilder();
            logTotal = -1;
            sendRaw("log");                    // answered by LOG BEGIN ... LOG END, not OK
            main.postDelayed(logStall, LOG_STALL_MS);
        } else if (needRead) {
            needRead = false;
            send("r", 'r');
        } else if (!known) {
            return;
        } else if (desired >= 0 && desired != confirmed) {
            sentValue = desired;
            send("m " + desired, 'm');
        } else if (subDesired >= 0 && subDesired != subConfirmed) {
            subSent = subDesired;
            send("s " + subDesired, 's');
        } else if (inputWanted >= 0 && inputWanted != inputDone) {
            inputSent = inputWanted;
            send("i " + inputWanted, 'i');
        }
    }

    // Notifications arrive in 20-byte pieces; reassemble whole lines first.
    void onBytes(byte[] b) {
        lineBuf.append(new String(b, StandardCharsets.US_ASCII));
        int i;
        while ((i = indexOfEol(lineBuf)) >= 0) {
            String line = lineBuf.substring(0, i).trim();
            lineBuf.delete(0, i + 1);
            if (!line.isEmpty()) onLine(line);
        }
        if (lineBuf.length() > 512) lineBuf.setLength(0);
    }

    static int indexOfEol(CharSequence s) {
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '\r' || c == '\n') return i;
        }
        return -1;
    }

    // ---- log download ---------------------------------------------------------
    static final long LOG_STALL_MS = 15000;      // no line for this long: give up

    final Runnable logStall = () -> finishLog(null, "The log download stalled");

    /** Lines between LOG BEGIN and LOG END are the log, never protocol replies. */
    boolean onLogLine(String line) {
        if (line.startsWith("LOG BEGIN")) {
            try { logTotal = Integer.parseInt(line.substring(9).trim()); }
            catch (NumberFormatException e) { logTotal = 0; }
        } else if (line.equals("LOG END")) {
            finishLog(logText.toString(), null);
            return true;
        } else if (logTotal >= 0) {
            logText.append(line).append('\n');
        } else {
            return false;                       // not started yet: an ordinary reply
        }
        main.removeCallbacks(logStall);
        main.postDelayed(logStall, LOG_STALL_MS);
        if (logListener != null) logListener.onLogProgress(logText.length(), logTotal);
        return true;
    }

    void finishLog(String text, String why) {
        logPending = false;
        main.removeCallbacks(logStall);
        LogListener l = logListener;
        logListener = null;
        if (l != null) {
            if (text != null) l.onLogDone(text);
            else l.onLogFailed(why);
        }
        pump();
    }

    void onLine(String line) {
        if (logPending && onLogLine(line)) return;

        // The help line names the commands this firmware has. "log" means v8 or
        // later, which also takes the phone's clock for its log timestamps.
        // v8.2 and later: "firmware v8.2" from '?', "firmware v8.2   name ..." from 'd'.
        if (line.startsWith("firmware v")) {
            String v = line.substring(9).trim().split("\\s+")[0];
            if (!v.equals(firmware)) { firmware = v; notifyUi(); }
            return;
        }
        // Older firmware names no version, but its 'd' output differs: v8 reports
        // "amp line up/down", v8.1 "amp answering yes/no".
        if (firmware == null && line.startsWith("amp line ")) { firmware = "v8"; notifyUi(); return; }
        if (firmware == null && line.startsWith("amp answering ")) { firmware = "v8.1"; notifyUi(); return; }

        if (line.startsWith("r | e")) {
            supportsLog = line.contains("log");
            if (!supportsLog) firmware = "v7 or older";
            if (supportsLog && !clockSent) {
                clockSent = true;
                send("t " + System.currentTimeMillis() / 1000, 't');
            }
            notifyUi();
            return;
        }

        // The ESP32 watches the amp's TX line and says when it changes. After a
        // start the levels may differ from before, so read them again.
        if (line.equals("amp on")) {
            setStatus("Amp switched on, reading it\u2026");
            if (!owed.contains('r')) needRead = true;
            pump();
            return;
        }
        if (line.equals("amp off")) {
            setStatus("The amp is off or asleep");
            return;
        }

        // "master 0..60, sub 0..23, input 0..2"
        Matcher m = CEILING.matcher(line);
        if (m.find()) {
            int ceiling = Integer.parseInt(m.group(1));
            if (ceiling > 0 && ceiling != max) {
                max = ceiling;
                if (known) installVolume();
            }
            maxSub = Integer.parseInt(m.group(2));
            // v8.2 and later follow this line with their version. If none has come
            // shortly, ask 'd', whose output tells v8 and v8.1 apart.
            if (supportsLog && firmware == null) {
                main.removeCallbacks(fwProbe);
                main.postDelayed(fwProbe, 1500);
            }
            notifyUi();
            return;
        }

        // "master=30 (0x1E)  sub=23 (0x17)"
        m = LEVELS.matcher(line);
        if (m.find()) {
            confirmed = Integer.parseInt(m.group(1));
            lastSync = System.currentTimeMillis();
            desired = clamp(confirmed + pendingSteps);
            pendingSteps = 0;
            int sub = Integer.parseInt(m.group(2));
            if (subDesired < 0 || subDesired == subConfirmed) subDesired = sub;   // no change pending
            subConfirmed = sub;
            if (!known) {
                known = true;
                installVolume();
                session.setActive(true);
            }
            setStatus("Connected to " + deviceName());
            showVolume();
            return;
        }

        if (line.equals("OK") || line.equals("FAILED")) {
            boolean ok = line.equals("OK");
            Character kind = owed.poll();
            if (owed.isEmpty()) main.removeCallbacks(replyTimeout);
            else { main.removeCallbacks(replyTimeout); main.postDelayed(replyTimeout, REPLY_MS); }
            if (kind != null && kind == 'm') {
                if (ok) {
                    confirmed = sentValue;
                    lastSync = System.currentTimeMillis();
                } else {
                    // Refused or not acknowledged by the amp: re-read rather than guess.
                    desired = confirmed;
                    needRead = true;
                }
            } else if (kind != null && kind == 's') {
                if (ok) subConfirmed = subSent;
                else { subDesired = subConfirmed; needRead = true; }
            } else if (kind != null && kind == 'i') {
                if (ok) inputDone = inputSent;
                else inputWanted = inputDone;
            } else if (kind != null && kind == 'r' && !ok) {
                pendingSteps = 0;
                setStatus("The amp did not answer a level read");
                // Without a first reading the controls stay disabled, so keep asking
                // rather than leave the app stuck until it is restarted.
                if (!known) {
                    main.removeCallbacks(retryRead);
                    main.postDelayed(retryRead, 3000);
                }
            } else if (kind != null && kind == 'h') {
                setStatus(ok ? "Start-up sequence done" : "The amp did not accept the start-up sequence");
                if (!owed.contains('r')) needRead = true;
            }
            showVolume();
            pump();
        }
        // Anything else (REFUSED lines, trace output before quiet mode) is ignored;
        // the FAILED that follows a refusal is what counts.
    }

    int clamp(int v) { return Math.max(0, Math.min(max, v)); }

    // ---- volume buttons -----------------------------------------------------
    void installVolume() {
        int now = Math.max(0, shown());
        volume = new VolumeProvider(VolumeProvider.VOLUME_CONTROL_ABSOLUTE, max, now) {
            @Override public void onAdjustVolume(int direction) {
                main.post(() -> step(direction));
            }
            @Override public void onSetVolumeTo(int v) {
                main.post(() -> setMaster(v));
            }
        };
        session.setPlaybackToRemote(volume);
    }

    void showVolume() {
        if (volume != null && shown() >= 0) volume.setCurrentVolume(shown());
        notifyUi();
    }

    // ---- status ---------------------------------------------------------------
    void setStatus(String s) {
        status = s;
        notifyUi();
    }

    void notifyUi() {
        if (listener != null) listener.onStateChanged();
        if (!stopped) {
            NotificationManager nm = getSystemService(NotificationManager.class);
            if (nm != null) nm.notify(NOTE_ID, buildNotification());
        }
    }

    Notification buildNotification() {
        PendingIntent open = PendingIntent.getActivity(this, 0,
                new Intent(this, MainActivity.class), PendingIntent.FLAG_IMMUTABLE);
        PendingIntent stop = PendingIntent.getService(this, 1,
                new Intent(this, AmpService.class).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE);
        String title = known && shown() >= 0
                ? "Volume " + shown() + " / " + max
                : "SCP Remote";
        return new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
                .setContentTitle(title)
                .setContentText(status)
                .setContentIntent(open)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .addAction(new Notification.Action.Builder(
                        Icon.createWithResource(this, android.R.drawable.ic_menu_close_clear_cancel),
                        "Stop", stop).build())
                .build();
    }
}
