/* SPDX-License-Identifier: GPL-2.0-only */
package io.github.switchframetap.tv;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.media.AudioManager;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.Locale;

public final class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final int INK = Color.rgb(9,17,28), WHITE = Color.rgb(237,244,250), GREEN = Color.rgb(87,219,197);
    private final Handler handler = new Handler(Looper.getMainLooper());
    private SharedPreferences prefs;
    private FrameLayout root;
    private SurfaceView picture;
    private ScrollView panel;
    private TextView status, hostInfo, hud;
    private EditText ip;
    private Button networkButton, usbButton, connectButton, disconnectButton, closePanelButton;
    private CheckBox mute, stable, showStats;
    private boolean usbMode, resumed, surfaceReady, requested, manualMenu, panelDismissed;
    private StreamEngine engine;
    private String message = "Choose a connection and select Connect.";
    private long sampledNs, sampledFrames, sampledBytes, sampledReceived, sampledDecoded, sampledAudio;
    private AudioManager audioManager;
    private boolean focusGranted;
    private AudioManager.OnAudioFocusChangeListener audioFocus;
    private AudioFocusRequest audioFocusRequest;
    private int focusSerial;
    private final BroadcastReceiver permissionReceiver = new BroadcastReceiver() {
        @Override public void onReceive(Context context, Intent intent) {
            if (!usbAction().equals(intent.getAction())) return;
            UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            boolean granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                    || (device != null && ((UsbManager)getSystemService(USB_SERVICE)).hasPermission(device));
            if (!granted) {
                requested = false; stopEngine();
                setMessage("USB access denied. Select Connect to request permission again.");
            }
        }
    };
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        prefs = getSharedPreferences("viewer", MODE_PRIVATE);
        usbMode = prefs.getBoolean("usb", false);
        audioManager = (AudioManager)getSystemService(AUDIO_SERVICE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        immersive();
        root = new FrameLayout(this); root.setBackgroundColor(INK); setContentView(root);
        picture = new SurfaceView(this);
        root.addView(picture, new FrameLayout.LayoutParams(-1, -1, Gravity.CENTER));
        picture.getHolder().addCallback(this);
        root.addOnLayoutChangeListener((v,l,t,r,b,ol,ot,or,ob) -> {
            if (r-l != or-ol || b-t != ob-ot) fitPicture();
        });
        hud = text("", 13, WHITE); hud.setPadding(dp(14), dp(10), dp(14), dp(10));
        hud.setBackgroundColor(Color.argb(205,9,17,28)); hud.setVisibility(View.GONE);
        root.addView(hud, new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.START));
        buildPanel();
        IntentFilter filter = new IntentFilter(usbAction());
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(permissionReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        else registerReceiver(permissionReceiver, filter);
        updateMode(); handler.post(tick);
    }
    private void buildPanel() {
        panel = new ScrollView(this); panel.setFillViewport(false);
        panel.setBackground(round(Color.rgb(16,28,43), Color.rgb(41,62,80)));
        panel.setPadding(dp(28), dp(18), dp(28), dp(18));
        FrameLayout.LayoutParams layout = new FrameLayout.LayoutParams(
                Math.min(dp(740), getResources().getDisplayMetrics().widthPixels - dp(48)), -1, Gravity.CENTER);
        layout.topMargin = layout.bottomMargin = dp(24);
        root.addView(panel, layout);
        LinearLayout body = new LinearLayout(this); body.setOrientation(LinearLayout.VERTICAL);
        panel.addView(body, new ScrollView.LayoutParams(-1,-2));
        LinearLayout heading = row(); heading.setGravity(Gravity.CENTER_VERTICAL); body.addView(heading);
        TextView title = text("Switch Frame Tap TV", 28, WHITE); title.setTypeface(null, Typeface.BOLD);
        heading.addView(title, new LinearLayout.LayoutParams(0,-2,1));
        closePanelButton = button("Close panel", v -> dismissPanel());
        closePanelButton.setContentDescription("Close the panel and show the video without disconnecting");
        heading.addView(closePanelButton, new LinearLayout.LayoutParams(dp(145),dp(48)));
        addText(body, "Video and audio from your Switch · direct playback", 14, GREEN);
        addText(body, Build.MANUFACTURER + " " + Build.MODEL + " · Android " + Build.VERSION.RELEASE, 12, Color.LTGRAY);
        status = text(message, 16, WHITE); status.setPadding(0,dp(14),0,dp(10)); body.addView(status);
        LinearLayout modes = row(); body.addView(modes);
        networkButton = button("Network", v -> chooseMode(false)); usbButton = button("USB", v -> chooseMode(true));
        addButton(modes, networkButton); addButton(modes, usbButton);
        ip = new EditText(this); ip.setTextColor(WHITE); ip.setHintTextColor(Color.LTGRAY);
        ip.setSingleLine(true); ip.setTextSize(17); ip.setHint("Switch IP address · leave blank for automatic discovery");
        ip.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_NORMAL);
        ip.setText(prefs.getString("ip", "")); ip.setSelectAllOnFocus(true); body.addView(ip);
        hostInfo = text("", 13, Color.LTGRAY); body.addView(hostInfo);
        LinearLayout actions = row(); body.addView(actions);
        connectButton = button("Connect", v -> connect());
        disconnectButton = button("Disconnect", v -> { requested = false; stopEngine(); manualMenu = false; panelDismissed = false; setMessage("Disconnected."); showPanel(true); });
        addButton(actions, connectButton); addButton(actions, disconnectButton);
        mute = check("Mute audio", prefs.getBoolean("mute", false)); body.addView(mute);
        mute.setOnCheckedChangeListener((v, checked) -> {
            prefs.edit().putBoolean("mute", checked).apply();
            if (engine != null) engine.mute(checked || !focusGranted);
        });
        stable = check("Stable audio (larger buffer to reduce dropouts)", prefs.getBoolean("stable_audio", false)); body.addView(stable);
        stable.setOnCheckedChangeListener((v, checked) -> {
            prefs.edit().putBoolean("stable_audio", checked).apply();
            if (requested) { stopEngine(); startEngine(); }
        });
        showStats = check("Show statistics", prefs.getBoolean("stats", false)); body.addView(showStats);
        showStats.setOnCheckedChangeListener((v, checked) -> {
            prefs.edit().putBoolean("stats", checked).apply(); hud.setVisibility(checked && engine != null ? View.VISIBLE : View.GONE);
        });
        addText(body, "Choose quality and resolution on the console. The app preserves the original video.", 13, Color.LTGRAY);
        addText(body, "OK / Menu: controls · Back: return to video\nKeep your game controller connected to the Switch.", 13, GREEN);
        addText(body, "Preview 0.1.2 · GPL-2.0 · Tomas Uribe / switch-frame-tap", 11, Color.LTGRAY);
        networkButton.requestFocus();
    }
    private void chooseMode(boolean usb) {
        requested = false; stopEngine(); usbMode = usb;
        panelDismissed = false;
        prefs.edit().putBoolean("usb", usb).apply();
        updateMode(); setMessage("Choose the same connection mode on the Switch, then select Connect.");
    }
    private void updateMode() {
        networkButton.setText(usbMode ? "Network" : "Network ✓"); usbButton.setText(usbMode ? "USB ✓" : "USB");
        ip.setVisibility(usbMode ? View.GONE : View.VISIBLE);
        if (usbMode) hostInfo.setText(getPackageManager().hasSystemFeature(PackageManager.FEATURE_USB_HOST)
                ? "Switch → USB data cable → TV. Select USB viewer mode on the console."
                : "This device does not report USB host support. Use Network instead.");
        else hostInfo.setText("Same local network · TCP 9950 · close the PC viewer. Only one viewer can connect.");
    }
    private void connect() {
        String address = ip.getText().toString().trim();
        if (!usbMode && !address.isEmpty() && !SftProtocol.ipv4(address)) {
            setMessage("Enter a valid IPv4 address, such as 192.168.1.50, or leave the field blank."); ip.requestFocus(); return;
        }
        ((android.view.inputmethod.InputMethodManager)getSystemService(INPUT_METHOD_SERVICE))
                .hideSoftInputFromWindow(ip.getWindowToken(), 0);
        prefs.edit().putString("ip", address).apply();
        requested = true; manualMenu = false; panelDismissed = false; stopEngine(); startEngine();
    }
    private void startEngine() {
        if (!requested || !resumed || !surfaceReady || engine != null) return;
        final int serial = ++focusSerial;
        audioFocus = value -> {
            if (serial != focusSerial) return; // ignore delayed callbacks from a previous connection
            focusGranted = value == AudioManager.AUDIOFOCUS_GAIN;
            if (engine != null) engine.mute(mute.isChecked() || !focusGranted);
        };
        if (Build.VERSION.SDK_INT >= 26) {
            audioFocusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                    .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE).build())
                    .setOnAudioFocusChangeListener(audioFocus, handler).setWillPauseWhenDucked(true).build();
            focusGranted = audioManager.requestAudioFocus(audioFocusRequest) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
        } else {
            focusGranted = audioManager.requestAudioFocus(audioFocus, AudioManager.STREAM_MUSIC,
                    AudioManager.AUDIOFOCUS_GAIN) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
        }
        final StreamEngine[] identity = new StreamEngine[1];
        StreamEngine.Listener listener = new StreamEngine.Listener() {
            @Override public void status(String value) { handler.post(() -> { if (engine == identity[0]) setMessage(value); }); }
            @Override public void connectedAddress(String value) {
                handler.post(() -> { if (engine == identity[0]) prefs.edit().putString("last_ip", value).apply(); });
            }
            @Override public void usbPermission(UsbDevice device) {
                handler.post(() -> {
                    if (engine != identity[0] || !resumed) return;
                    PendingIntent result = PendingIntent.getBroadcast(MainActivity.this, 0,
                            new Intent(usbAction()).setPackage(getPackageName()), PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
                    try { ((UsbManager)getSystemService(USB_SERVICE)).requestPermission(device, result); }
                    catch (RuntimeException e) { setMessage("Could not request USB permission: " + e.getMessage()); }
                });
            }
        };
        engine = new StreamEngine(this, picture.getHolder().getSurface(), usbMode, prefs.getString("ip", ""),
                prefs.getString("last_ip", ""), stable.isChecked(), mute.isChecked() || !focusGranted, listener);
        identity[0] = engine; sampledNs = System.nanoTime();
        sampledBytes = sampledFrames = sampledReceived = sampledDecoded = sampledAudio = 0;
        showPanel(true); setMessage("Starting connection…"); engine.start();
    }
    private void stopEngine() {
        StreamEngine old = engine; engine = null;
        focusSerial++;
        if (old != null) old.stop();
        hud.setVisibility(View.GONE);
        if (Build.VERSION.SDK_INT >= 26 && audioFocusRequest != null) audioManager.abandonAudioFocusRequest(audioFocusRequest);
        else if (audioFocus != null) audioManager.abandonAudioFocus(audioFocus);
        audioFocusRequest = null; audioFocus = null; focusGranted = false;
    }
    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (engine != null) {
                StreamStats s = engine.stats;
                long now = System.nanoTime();
                boolean active = s.lastRenderedNs > 0 && now - s.lastRenderedNs < 1_500_000_000L;
                if (active && !manualMenu) showPanel(false);
                else if (!active && !panelDismissed) {
                    if (panel.getVisibility() != View.VISIBLE) {
                        setMessage(s.decoderErrors > 0 ? message : "No recent video. Open or return to a game on the Switch.");
                        showPanel(true);
                    }
                }
                fitPicture();
                double seconds = (now - sampledNs) / 1_000_000_000.0;
                if (seconds >= 1) {
                    double fps = (s.rendered - sampledFrames) / seconds;
                    double mbps = (s.bytes - sampledBytes) * 8 / seconds / 1_000_000;
                    double rx = (s.received - sampledReceived) / seconds;
                    double decoded = (s.decoded - sampledDecoded) / seconds;
                    double audioKb = (s.audioBytes - sampledAudio) / seconds / 1000;
                    hud.setText(String.format(Locale.ROOT,
                            "%s · %d×%d · %.1f Mbps\nVideo RX %.1f · decoded %.1f · shown %.1f FPS\nQueue + decode %.1f ms · console age %.1f ms\nAudio RX %.0f kB/s · played %.0f ms · queued %.0f ms\nAudio %s · TV buffer %.0f ms · %s\nErrors %d · dropped %d/%d · adjusted PTS %d\n%s",
                            s.transport, s.width, s.height, mbps, rx, decoded, fps, s.decoderMs, s.consoleMs,
                            audioKb, s.audioPlayedFrames / 48.0, s.audioQueuedMs,
                            s.audioMuted ? "muted" : "active", s.audioBufferMs,
                            focusGranted ? "focus granted" : "no audio focus", s.decoderErrors, s.skipped,
                            s.videoDropped, s.unmatchedPts, s.decoder + " · " + s.audio));
                    sampledNs = now; sampledFrames = s.rendered; sampledBytes = s.bytes;
                    sampledReceived = s.received; sampledDecoded = s.decoded; sampledAudio = s.audioBytes;
                }
                hud.setVisibility(showStats.isChecked() ? View.VISIBLE : View.GONE);
            }
            handler.postDelayed(this, 250);
        }
    };
    private void fitPicture() {
        if (root.getWidth() == 0 || root.getHeight() == 0) return;
        int w = engine == null ? 16 : Math.max(16, engine.stats.width);
        int h = engine == null ? 9 : Math.max(9, engine.stats.height);
        double ratio = (double)w/h;
        int fitW = root.getWidth(), fitH = (int)(fitW / ratio);
        if (fitH > root.getHeight()) { fitH = root.getHeight(); fitW = (int)(fitH * ratio); }
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams)picture.getLayoutParams();
        if (lp.width != fitW || lp.height != fitH) { lp.width = fitW; lp.height = fitH; picture.setLayoutParams(lp); }
    }
    private void setMessage(String value) { message = value; status.setText(value); }
    private void showPanel(boolean value) {
        if (value && panel.getVisibility() != View.VISIBLE) { panel.setVisibility(View.VISIBLE); connectButton.requestFocus(); }
        else if (!value) panel.setVisibility(View.GONE);
    }
    private void dismissPanel() { manualMenu = false; panelDismissed = true; showPanel(false); }
    @Override public boolean onKeyDown(int code, KeyEvent event) {
        if (event.getRepeatCount() == 0 && (code == KeyEvent.KEYCODE_MENU || code == KeyEvent.KEYCODE_DPAD_CENTER
                || code == KeyEvent.KEYCODE_ENTER) && panel.getVisibility() != View.VISIBLE) {
            manualMenu = true; panelDismissed = false; showPanel(true); return true;
        }
        return super.onKeyDown(code, event);
    }
    @Override public void onBackPressed() {
        if (panel.getVisibility() != View.VISIBLE) {
            manualMenu = true; panelDismissed = false; showPanel(true);
        } else if (engine != null) {
            dismissPanel();
        } else { requested = false; stopEngine(); super.onBackPressed(); }
    }
    @Override protected void onResume() { super.onResume(); resumed = true; immersive(); startEngine(); }
    @Override protected void onPause() { resumed = false; stopEngine(); super.onPause(); }
    @Override protected void onDestroy() { handler.removeCallbacksAndMessages(null); unregisterReceiver(permissionReceiver); super.onDestroy(); }
    @Override public void surfaceCreated(SurfaceHolder holder) { surfaceReady = true; startEngine(); }
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) { }
    @Override public void surfaceDestroyed(SurfaceHolder holder) { surfaceReady = false; stopEngine(); }
    @Override public void onWindowFocusChanged(boolean focus) { super.onWindowFocusChanged(focus); if (focus) immersive(); }
    private void immersive() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }
    private String usbAction() { return getPackageName() + ".USB_PERMISSION"; }
    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    private TextView text(String value, int size, int color) {
        TextView view = new TextView(this); view.setText(value); view.setTextSize(size); view.setTextColor(color); return view;
    }
    private void addText(LinearLayout body, String value, int size, int color) {
        TextView view = text(value,size,color); view.setPadding(0,dp(4),0,dp(4)); body.addView(view);
    }
    private LinearLayout row() { LinearLayout row = new LinearLayout(this); row.setOrientation(LinearLayout.HORIZONTAL); return row; }
    private void addButton(LinearLayout row, Button button) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0,dp(48),1); lp.setMargins(dp(3),dp(5),dp(3),dp(5)); row.addView(button,lp);
    }
    private Button button(String value, View.OnClickListener click) {
        Button b = new Button(this); b.setText(value); b.setTextSize(16); b.setAllCaps(false); b.setTextColor(WHITE);
        StateListDrawable background = new StateListDrawable();
        background.addState(new int[]{android.R.attr.state_focused}, round(Color.rgb(24,91,89), GREEN));
        background.addState(new int[]{android.R.attr.state_pressed}, round(Color.rgb(24,91,89), GREEN));
        background.addState(new int[]{}, round(Color.rgb(26,43,60), Color.rgb(62,84,102)));
        b.setBackground(background); b.setOnClickListener(click); return b;
    }
    private CheckBox check(String label, boolean checked) {
        CheckBox c = new CheckBox(this); c.setText(label); c.setTextColor(WHITE); c.setTextSize(14); c.setChecked(checked);
        c.setMinHeight(dp(40)); return c;
    }
    private GradientDrawable round(int fill, int stroke) {
        GradientDrawable d = new GradientDrawable(); d.setColor(fill); d.setCornerRadius(dp(12)); d.setStroke(dp(2),stroke); return d;
    }
}
