// SPDX-License-Identifier: Apache-2.0
//
// The Java an Android app built with the Sonore SDK carries: what the C++ side
// cannot reach through JNI on framework classes alone, because the framework
// answers it through CALLBACKS -- objects implementing its interfaces, which
// only Java can make.
//
//  - MIDI. MidiManager hands a device over in onDeviceOpened, and says one was
//    plugged in or out through a DeviceCallback. Each opened device goes to
//    native code, which reads it with AMidi (android_midi.h).
//  - The plugin's page. A WebView's JavaScript reaches the app through an
//    object with @JavascriptInterface methods; navigation is refused by a
//    WebViewClient. The page is in a window of its own -- a PopupWindow over
//    the activity, inside the system bars -- because NativeActivity takes the
//    activity window's surface for native drawing, and a view added to that
//    window is never drawn (webview_android.h).
//
// Compiled by sonore_add_standalone into the APK's classes.dex. Everything is
// static and called from native code on the main thread, except the page's
// post(), which the WebView calls on its own thread.
package com.sonorie.sonore;

import android.app.Activity;
import android.content.Context;
import android.graphics.Color;
import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.PopupWindow;

import java.util.HashSet;
import java.util.Set;

public final class SonoreBridge {
    private SonoreBridge() {}

    // ── MIDI ────────────────────────────────────────────────────────────────

    /** A device opened: native code takes its output ports from here. */
    static native void midiDeviceOpened(Object device, String name, int id);
    /** A device unplugged. */
    static native void midiDeviceRemoved(int id);

    private static final Set<Integer> opened = new HashSet<>();

    /** Open every device that sends MIDI, now and whenever one is plugged in. */
    public static void startMidi(Activity activity) {
        final MidiManager manager = (MidiManager) activity.getSystemService(Context.MIDI_SERVICE);
        if (manager == null) return;
        final Handler handler = new Handler(Looper.getMainLooper());
        manager.registerDeviceCallback(new MidiManager.DeviceCallback() {
            @Override
            public void onDeviceAdded(MidiDeviceInfo info) {
                open(manager, info, handler);
            }

            @Override
            public void onDeviceRemoved(MidiDeviceInfo info) {
                synchronized (opened) {
                    opened.remove(info.getId());
                }
                midiDeviceRemoved(info.getId());
            }
        }, handler);
        for (MidiDeviceInfo info : manager.getDevices()) open(manager, info, handler);
    }

    private static void open(MidiManager manager, final MidiDeviceInfo info, Handler handler) {
        // An OUTPUT port is one the device sends on: what a keyboard has.
        if (info.getOutputPortCount() < 1) return;
        synchronized (opened) {
            if (!opened.add(info.getId())) return;
        }
        manager.openDevice(info, new MidiManager.OnDeviceOpenedListener() {
            @Override
            public void onDeviceOpened(MidiDevice device) {
                if (device != null) midiDeviceOpened(device, nameOf(info), info.getId());
            }
        }, handler);
    }

    private static String nameOf(MidiDeviceInfo info) {
        Bundle p = info.getProperties();
        String name = p != null ? p.getString(MidiDeviceInfo.PROPERTY_NAME) : null;
        return name != null ? name : "MIDI device";
    }

    // ── The plugin's page ───────────────────────────────────────────────────

    /** A message the page posted, as the JSON the bridge sends. */
    static native void pageMessage(long owner, String json);

    /** What the page's bridge script calls: window.sonoreAndroid.post(json). */
    static final class PageChannel {
        private final long owner;

        PageChannel(long owner) {
            this.owner = owner;
        }

        @JavascriptInterface
        public void post(String json) {
            if (json != null) pageMessage(owner, json);
        }
    }

    /** The page, over the activity, at x, y, w x h device pixels. */
    public static WebView createWebView(Activity activity, long owner, String html, int x, int y, int w, int h) {
        WebView view = new WebView(activity);
        WebSettings settings = view.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        settings.setMediaPlaybackRequiresUserGesture(false);
        // The page stays the page: no link leaves it, no window opens.
        view.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView v, WebResourceRequest request) {
                return true;
            }
        });
        view.setBackgroundColor(Color.BLACK);
        view.setOverScrollMode(View.OVER_SCROLL_NEVER);
        view.addJavascriptInterface(new PageChannel(owner), "sonoreAndroid");
        // Focusable, so a text field in the page gets the soft keyboard; the
        // Back key then dismisses nothing (the activity ends as it would).
        final PopupWindow popup = new PopupWindow(view, w, h, true);
        popup.setClippingEnabled(false);
        popup.setOutsideTouchable(false);
        view.setTag(popup);
        final View anchor = activity.getWindow().getDecorView();
        final int px = x, py = y;
        // Posted, so the activity's window is attached and has a token.
        anchor.post(new Runnable() {
            @Override
            public void run() {
                if (!popup.isShowing()) popup.showAtLocation(anchor, Gravity.NO_GRAVITY, px, py);
            }
        });
        view.loadDataWithBaseURL("https://appassets.sonore.local/", html, "text/html", "utf-8", null);
        return view;
    }

    /** Move and size the page, in device pixels. */
    public static void placeWebView(WebView view, int x, int y, int w, int h) {
        Object tag = view.getTag();
        if (tag instanceof PopupWindow && ((PopupWindow) tag).isShowing()) ((PopupWindow) tag).update(x, y, w, h);
    }

    public static void evaluate(WebView view, String script) {
        view.evaluateJavascript(script, null);
    }

    public static void setVisible(WebView view, boolean visible) {
        view.setVisibility(visible ? View.VISIBLE : View.GONE);
    }

    public static void destroyWebView(WebView view) {
        Object tag = view.getTag();
        if (tag instanceof PopupWindow) ((PopupWindow) tag).dismiss();
        ViewGroup parent = (ViewGroup) view.getParent();
        if (parent != null) parent.removeView(view);
        view.removeJavascriptInterface("sonoreAndroid");
        view.destroy();
    }
}
