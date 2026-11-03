package io.github.gajdi88.scpremote;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

/**
 * A vertical fader, like a mixing desk's, for the landscape layout.
 *
 * Dragging moves the level by how far the finger travels, not to where it
 * lands: touching the top of the track does not jump the amp to full volume.
 * One full-height drag covers the whole range. A tap without movement changes
 * nothing.
 */
public class Fader extends View {

    /** Called on the main thread while the user drags. */
    public interface Listener {
        void onStart();
        void onChange(int value);
        void onStop(int value);
    }

    private final float density;
    private final Paint track = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint tick = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint cap = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint grip = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF r = new RectF();

    private int max = 60, value;
    private Listener listener;
    private float startY;
    private int startValue;
    private boolean dragging;

    public Fader(Context c, int trackColor, int fillColor, int capColor) {
        super(c);
        density = c.getResources().getDisplayMetrics().density;
        track.setColor(trackColor);
        fill.setColor(fillColor);
        tick.setColor(trackColor);
        tick.setStrokeWidth(dp(2));
        cap.setColor(capColor);
        grip.setColor(trackColor);
        grip.setStrokeWidth(dp(2));
    }

    public void setListener(Listener l) { listener = l; }

    public void setMax(int m) {
        if (m <= 0 || m == max) return;
        max = m;
        value = Math.min(value, max);
        invalidate();
    }

    /** Sets the level shown, without telling the listener. Ignored mid-drag. */
    public void setValue(int v) {
        if (dragging) return;
        v = Math.max(0, Math.min(max, v));
        if (v != value) { value = v; invalidate(); }
    }

    public int getValue() { return value; }

    public int getMaxValue() { return max; }

    private float dp(float v) { return v * density; }

    // The thumb's travel: inset top and bottom by half its height.
    private float capH() { return dp(26); }
    private float top() { return getPaddingTop() + capH() / 2; }
    private float bottom() { return getHeight() - getPaddingBottom() - capH() / 2; }
    private float levelY() { return bottom() - (bottom() - top()) * value / (float) max; }

    @Override protected void onDraw(Canvas c) {
        float cx = getWidth() / 2f;
        float tw = Math.min(getWidth() * 0.34f, dp(22));     // track width
        float t = top(), b = bottom(), y = levelY();

        // Track, then the filled part from the bottom up to the level.
        r.set(cx - tw / 2, t, cx + tw / 2, b);
        round(c, r, tw / 2, track);
        r.set(cx - tw / 2, y, cx + tw / 2, b);
        round(c, r, tw / 2, fill);

        // Ticks at quarters, both sides, as on a desk.
        float half = Math.min(getWidth() * 0.42f, dp(30));
        for (int i = 0; i <= 4; i++) {
            float ty = b - (b - t) * i / 4f;
            float len = (i == 0 || i == 4) ? dp(8) : dp(5);
            c.drawLine(cx - half, ty, cx - half + len, ty, tick);
            c.drawLine(cx + half - len, ty, cx + half, ty, tick);
        }

        // The cap: wider than the track, with a grip line at the exact level.
        float capW = Math.min(getWidth() * 0.78f, dp(58));
        r.set(cx - capW / 2, y - capH() / 2, cx + capW / 2, y + capH() / 2);
        round(c, r, dp(7), cap);
        c.drawLine(cx - capW / 2 + dp(9), y, cx + capW / 2 - dp(9), y, grip);
    }

    private static void round(Canvas c, RectF r, float radius, Paint p) {
        c.drawRoundRect(r, radius, radius, p);
    }

    @SuppressLint("ClickableViewAccessibility")
    @Override public boolean onTouchEvent(MotionEvent e) {
        if (!isEnabled()) return false;
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                // Keep a surrounding scroll view from taking the gesture.
                if (getParent() != null) getParent().requestDisallowInterceptTouchEvent(true);
                startY = e.getY();
                startValue = value;
                dragging = true;
                if (listener != null) listener.onStart();
                return true;
            case MotionEvent.ACTION_MOVE: {
                float perStep = (bottom() - top()) / max;
                int v = startValue + Math.round((startY - e.getY()) / perStep);
                v = Math.max(0, Math.min(max, v));
                if (v != value) {
                    value = v;
                    invalidate();
                    if (listener != null) listener.onChange(v);
                }
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                dragging = false;
                if (listener != null) listener.onStop(value);
                return true;
        }
        return false;
    }
}
