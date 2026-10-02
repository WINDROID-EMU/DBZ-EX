package com.dbz.butoden;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

public class MainActivity extends Activity implements SurfaceHolder.Callback {

    static {
        System.loadLibrary("dbz_native");
    }

    private SurfaceView surfaceView;

    // Native C++ methods
    public static native void nativeInit(String internalPath);
    public static native void nativeSurfaceCreated(Surface surface);
    public static native void nativeSurfaceChanged(int width, int height);
    public static native void nativeSurfaceDestroyed();
    public static native void nativeTouch(boolean pressed, float x, float y);
    public static native void nativeButton(int buttonMask, boolean pressed);
    public static native void nativeResume();
    public static native void nativePause();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Make window background transparent so SurfaceView is visible
        getWindow().setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(android.graphics.Color.TRANSPARENT));
        setContentView(R.layout.activity_main);

        // Keep screen on
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        hideSystemUI();

        // Initialize Native Core with storage path
        nativeInit(getFilesDir().getAbsolutePath());

        surfaceView = findViewById(R.id.render_surface);
        surfaceView.getHolder().setFormat(android.graphics.PixelFormat.TRANSLUCENT);
        surfaceView.setZOrderOnTop(true);
        surfaceView.getHolder().addCallback(this);

        // Auto-press Start/A after 2.5s and 4.5s to skip initial intro screen automatically
        new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
            nativeButton(1 | 8, true); // A + START
            new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> nativeButton(0, false), 200);
        }, 2500);

        new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
            nativeButton(1 | 8, true); // A + START
            new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> nativeButton(0, false), 200);
        }, 4500);

        surfaceView.setOnTouchListener((v, event) -> {
            int action = event.getActionMasked();
            boolean isPressed = (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_MOVE);
            float viewW = v.getWidth() > 0 ? v.getWidth() : 1.0f;
            float viewH = v.getHeight() > 0 ? v.getHeight() : 1.0f;

            // Calculate exact bottom screen viewport on right half (matching PicaGLES)
            float halfW = viewW / 2.0f;
            float targetRatioTop = 400.0f / 240.0f;
            float topH = halfW / targetRatioTop;
            if (topH > viewH) {
                topH = viewH;
            }
            float botH = topH;
            float botW = botH * (320.0f / 240.0f);
            if (botW > halfW) {
                botW = halfW;
                botH = botW * (240.0f / 320.0f);
            }
            float botX = halfW + (halfW - botW) / 2.0f;
            float botY = (viewH - botH) / 2.0f;

            float touchX = event.getX();
            float touchY = event.getY();

            if (touchX >= botX && touchX <= botX + botW && touchY >= botY && touchY <= botY + botH) {
                float tX = ((touchX - botX) / botW) * 320.0f;
                float tY = ((touchY - botY) / botH) * 240.0f;
                nativeTouch(isPressed, tX, tY);
            } else if (!isPressed) {
                nativeTouch(false, 0, 0);
            }

            // Also pulse A + START on touch to skip intro screens easily
            if (action == MotionEvent.ACTION_DOWN) {
                nativeButton(1 | 8, true); // BUTTON_A (1) | BUTTON_START (8)
            } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
                nativeButton(0, false);
            }
            return true;
        });
    }

    @Override
    public boolean onKeyDown(int keyCode, android.view.KeyEvent event) {
        if (keyCode == android.view.KeyEvent.KEYCODE_VOLUME_UP || keyCode == android.view.KeyEvent.KEYCODE_BUTTON_A || keyCode == android.view.KeyEvent.KEYCODE_ENTER) {
            nativeButton(1 | 8, true); // A + START
            return true;
        } else if (keyCode == android.view.KeyEvent.KEYCODE_VOLUME_DOWN || keyCode == android.view.KeyEvent.KEYCODE_BUTTON_B) {
            nativeButton(2, true); // B
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, android.view.KeyEvent event) {
        if (keyCode == android.view.KeyEvent.KEYCODE_VOLUME_UP || keyCode == android.view.KeyEvent.KEYCODE_BUTTON_A || keyCode == android.view.KeyEvent.KEYCODE_ENTER) {
            nativeButton(0, false);
            return true;
        } else if (keyCode == android.view.KeyEvent.KEYCODE_VOLUME_DOWN || keyCode == android.view.KeyEvent.KEYCODE_BUTTON_B) {
            nativeButton(0, false);
            return true;
        }
        return super.onKeyUp(keyCode, event);
    }

    private void hideSystemUI() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            final WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN
            );
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemUI();
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        nativeSurfaceCreated(holder.getSurface());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        nativeSurfaceChanged(width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeSurfaceDestroyed();
    }

    @Override
    protected void onResume() {
        super.onResume();
        nativeResume();
    }

    @Override
    protected void onPause() {
        super.onPause();
        nativePause();
    }
}
