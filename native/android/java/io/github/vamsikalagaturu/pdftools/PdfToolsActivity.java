package io.github.vamsikalagaturu.pdftools;

import android.app.NativeActivity;
import android.content.Intent;

/* NativeActivity drops intents delivered to a running instance (launchMode singleTask); keeping the latest one
 * lets the native side see a PDF opened from another app while the window is already up. */
public class PdfToolsActivity extends NativeActivity {
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
    }
}
