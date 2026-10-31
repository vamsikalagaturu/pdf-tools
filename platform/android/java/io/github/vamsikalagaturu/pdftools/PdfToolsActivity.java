package io.github.vamsikalagaturu.pdftools;

import android.app.NativeActivity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.print.PageRange;
import android.print.PrintAttributes;
import android.print.PrintDocumentAdapter;
import android.print.PrintDocumentInfo;
import android.print.PrintManager;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.text.InputType;
import android.graphics.Insets;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.webkit.MimeTypeMap;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.concurrent.Callable;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;

/* The native window plus what only Java can reach: the system document picker (Storage Access Framework; the app
 * has no storage permission), content:// I/O, the soft keyboard's text, the clipboard, printing and the system bar
 * insets. Called from the native thread (android_main.c, file_dialog_android.c, printing.c); every public method
 * takes and returns strings, null meaning "no" or "nothing". */
public class PdfToolsActivity extends NativeActivity {
    private static final int REQ_PICK = 1;
    private final Object lock = new Object();
    private String pickResult;        /* null while pending, "" when cancelled, else URIs separated by '\n' */

    /* NativeActivity drops intents delivered to a running instance (launchMode singleTask); keeping the latest
     * lets the native side see a PDF opened from another app while the window is up. */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
    }

    /* What covers the window, in pixels: "left top right bottom": the visible system bars, the display cutout and
     * the soft keyboard (the window does not resize for it: adjustNothing). */
    public String systemInsets() {
        WindowInsets w = getWindow().getDecorView().getRootWindowInsets();
        if (w == null) return null;
        Insets i = w.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout() | WindowInsets.Type.ime());
        return i.left + " " + i.top + " " + i.right + " " + i.bottom;
    }

    /* Run on the UI thread and wait: the clipboard wants a Looper thread on older releases. */
    private String onUi(Callable<String> job) {
        FutureTask<String> task = new FutureTask<>(job);
        runOnUiThread(task);
        try {
            return task.get(5, TimeUnit.SECONDS);
        } catch (Exception e) {
            return null;
        }
    }

    /* ---- soft keyboard ---- */

    /* code points, and these, for android_main.c's native_text() */
    private static final int IME_BACKSPACE = -1, IME_ENTER = -2, IME_DELETE = -3;
    private static native void nativeText(int[] codes);

    private TextSink sink;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        /* the window reaches into the camera cutout so the strip shows the app's background, not black; the
         * native side keeps its controls clear of it through systemInsets() */
        WindowManager.LayoutParams lp = getWindow().getAttributes();
        lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setAttributes(lp);
        sink = new TextSink();
        addContentView(sink, new ViewGroup.LayoutParams(1, 1));
    }

    public String showKeyboard() {
        runOnUiThread(() -> {
            sink.requestFocus();
            InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            imm.restartInput(sink);
            imm.showSoftInput(sink, 0);
        });
        return null;
    }

    public String hideKeyboard() {
        runOnUiThread(() -> {
            InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            imm.hideSoftInputFromWindow(sink.getWindowToken(), 0);
        });
        return null;
    }

    /* An invisible editor the keyboard talks to: NativeActivity's own view takes key events only, so composed
     * text (CJK, swipe, autocorrect) would never arrive. What the keyboard is composing is typed into the native
     * text field as it goes and replaced (backspaces, then the new text) when it changes or is committed. */
    private final class TextSink extends View {
        TextSink() {
            super(PdfToolsActivity.this);
            setFocusable(true);
            setFocusableInTouchMode(true);
        }

        @Override
        public boolean onCheckIsTextEditor() { return true; }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo info) {
            info.inputType = InputType.TYPE_CLASS_TEXT;
            info.imeOptions = EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI;
            return new SinkConnection(this);
        }
    }

    private static final class SinkConnection extends BaseInputConnection {
        private String composing = "";

        SinkConnection(View view) { super(view, false); }

        private static void send(int backspaces, CharSequence text) {
            String s = text == null ? "" : text.toString();
            int[] codes = new int[backspaces + s.codePointCount(0, s.length())];
            int k = 0;
            for (int i = 0; i < backspaces; i++) codes[k++] = IME_BACKSPACE;
            for (int i = 0; i < s.length(); i += Character.charCount(s.codePointAt(i))) {
                int c = s.codePointAt(i);
                codes[k++] = c == '\n' ? IME_ENTER : c;
            }
            if (codes.length > 0) nativeText(codes);
        }

        private void replaceComposing(CharSequence text) {
            send(composing.codePointCount(0, composing.length()), text);
        }

        @Override
        public boolean setComposingText(CharSequence text, int cursor) {
            replaceComposing(text);
            composing = text == null ? "" : text.toString();
            return true;
        }

        @Override
        public boolean commitText(CharSequence text, int cursor) {
            replaceComposing(text);
            composing = "";
            return true;
        }

        @Override
        public boolean finishComposingText() {
            composing = "";
            return true;
        }

        @Override
        public boolean deleteSurroundingText(int before, int after) {
            int[] codes = new int[before + after];
            for (int i = 0; i < before; i++) codes[i] = IME_BACKSPACE;
            for (int i = 0; i < after; i++) codes[before + i] = IME_DELETE;
            if (codes.length > 0) nativeText(codes);
            return true;
        }

        @Override
        public boolean performEditorAction(int action) {
            nativeText(new int[] {IME_ENTER});
            return true;
        }
    }

    /* ---- clipboard ---- */

    public String setClipboard(String text) {
        return onUi(() -> {
            ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
            cm.setPrimaryClip(ClipData.newPlainText("PDF Tools", text));
            return "1";
        });
    }

    public String clipboard() {
        return onUi(() -> {
            ClipboardManager cm = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
            ClipData clip = cm.getPrimaryClip();
            if (clip == null || clip.getItemCount() == 0) return null;
            CharSequence s = clip.getItemAt(0).coerceToText(this);
            return s == null ? null : s.toString();
        });
    }

    /* A link from a document, for whatever app handles it. */
    public String openUrl(String url) {
        return onUi(() -> {
            try {
                startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
                return "1";
            } catch (Exception e) {
                return null;
            }
        });
    }

    /* ---- sharing ---- */

    /* The share sheet for cache/share/<name>, which ShareProvider serves. */
    public String share(String name) {
        Uri uri = new Uri.Builder().scheme("content").authority(getPackageName() + ".share").appendPath(name).build();
        return onUi(() -> {
            Intent send = new Intent(Intent.ACTION_SEND);
            send.setType("application/pdf");
            send.putExtra(Intent.EXTRA_STREAM, uri);
            send.setClipData(ClipData.newRawUri(name, uri));
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(send, name));
            return "1";
        });
    }

    /* ---- printing ---- */

    /* Hand a PDF to the system print dialog, which renders it and picks printer, copies and pages. The file is
     * deleted when the job ends. */
    public String printPdf(String path, String name) {
        return onUi(() -> {
            PrintManager pm = (PrintManager) getSystemService(PRINT_SERVICE);
            pm.print(name, new PrintDocumentAdapter() {
                @Override
                public void onLayout(PrintAttributes old, PrintAttributes attrs, CancellationSignal cancel,
                                     LayoutResultCallback cb, Bundle extras) {
                    if (cancel.isCanceled()) {
                        cb.onLayoutCancelled();
                        return;
                    }
                    cb.onLayoutFinished(new PrintDocumentInfo.Builder(name)
                        .setContentType(PrintDocumentInfo.CONTENT_TYPE_DOCUMENT).build(), true);
                }

                @Override
                public void onWrite(PageRange[] pages, ParcelFileDescriptor dest, CancellationSignal cancel,
                                    WriteResultCallback cb) {
                    try (InputStream in = new FileInputStream(path);
                         OutputStream out = new FileOutputStream(dest.getFileDescriptor())) {
                        byte[] buf = new byte[65536];
                        for (int n; (n = in.read(buf)) > 0;) {
                            if (cancel.isCanceled()) {
                                cb.onWriteCancelled();
                                return;
                            }
                            out.write(buf, 0, n);
                        }
                        cb.onWriteFinished(new PageRange[] {PageRange.ALL_PAGES});
                    } catch (Exception e) {
                        cb.onWriteFailed(e.toString());
                    }
                }

                @Override
                public void onFinish() {
                    new File(path).delete();
                }
            }, null);
            return "1";
        });
    }

    /* ---- the picker ---- */

    /* mode: open, many, save or dir; mimes: space-separated; name: suggested file name for save. */
    public String pick(String mode, String mimes, String name) {
        Intent i;
        if (mode.equals("dir")) {
            i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            i.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        } else {
            String[] types = mimes == null || mimes.isEmpty() ? new String[] {"*/*"} : mimes.split(" ");
            i = new Intent(mode.equals("save") ? Intent.ACTION_CREATE_DOCUMENT : Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType(types.length == 1 ? types[0] : "*/*");
            if (types.length > 1) i.putExtra(Intent.EXTRA_MIME_TYPES, types);
            if (mode.equals("many")) i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            if (mode.equals("save") && name != null) i.putExtra(Intent.EXTRA_TITLE, name);
        }
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
            | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        synchronized (lock) { pickResult = null; }
        final Intent intent = i;
        runOnUiThread(() -> startPick(intent));
        return null;
    }

    private void startPick(Intent intent) {
        try {
            startActivityForResult(intent, REQ_PICK);
        } catch (Exception e) {
            synchronized (lock) { pickResult = ""; }
        }
    }

    public String pickResult() {
        synchronized (lock) {
            String r = pickResult;
            if (r != null) pickResult = null;
            return r;
        }
    }

    @Override
    protected void onActivityResult(int code, int result, Intent data) {
        if (code != REQ_PICK) {
            super.onActivityResult(code, result, data);
            return;
        }
        StringBuilder out = new StringBuilder();
        if (result == RESULT_OK && data != null) {
            int flags = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            ClipData clip = data.getClipData();
            if (clip != null) {
                for (int k = 0; k < clip.getItemCount(); k++) add(out, clip.getItemAt(k).getUri(), flags);
            } else if (data.getData() != null) add(out, data.getData(), flags);
        }
        synchronized (lock) { pickResult = out.toString(); }
    }

    private void add(StringBuilder out, Uri uri, int flags) {
        if (uri == null) return;
        try {
            getContentResolver().takePersistableUriPermission(uri, flags);
        } catch (Exception e) {
            /* not persistable: the grant lasts while this activity lives */
        }
        if (out.length() > 0) out.append('\n');
        out.append(uri.toString());
    }

    /* Keep access to a document handed over by another app (ACTION_VIEW), as far as the grant allows. */
    public String keep(String uri) {
        ContentResolver r = getContentResolver();
        try {
            r.takePersistableUriPermission(Uri.parse(uri),
                Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        } catch (Exception e) {
            try {
                r.takePersistableUriPermission(Uri.parse(uri), Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (Exception e2) {
                return null;
            }
        }
        return "1";
    }

    /* ---- documents ---- */

    public String displayName(String uri) {
        try (Cursor c = getContentResolver().query(Uri.parse(uri), new String[] {OpenableColumns.DISPLAY_NAME},
                 null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception e) {
            /* fall through */
        }
        return Uri.parse(uri).getLastPathSegment();
    }

    /* The usual extension for a document's type, for a name that came without one (the window picks the
     * reader by extension); null when the type is unknown. */
    public String extensionOf(String uri) {
        String type;
        try {
            type = getContentResolver().getType(Uri.parse(uri));
        } catch (Exception e) {
            return null;
        }
        if (type == null) return null;
        switch (type) {
        case "application/epub+zip": return "epub";
        case "application/x-mobipocket-ebook": return "mobi";
        case "application/x-fictionbook+xml": return "fb2";
        case "application/vnd.comicbook+zip": return "cbz";
        case "application/vnd.ms-xpsdocument": return "xps";
        case "application/oxps": return "oxps";
        default: return MimeTypeMap.getSingleton().getExtensionFromMimeType(type);
        }
    }

    /* Copy a document to a local file (written beside it, then renamed). Returns null, or the error. */
    public String copyIn(String uri, String path) {
        File part = new File(path + ".part");
        try (InputStream in = getContentResolver().openInputStream(Uri.parse(uri));
             OutputStream out = new FileOutputStream(part)) {
            if (in == null) return "no data";
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
        } catch (Exception e) {
            part.delete();
            return e.toString();
        }
        return part.renameTo(new File(path)) ? null : "rename failed";
    }

    /* Replace a document's content with a local file. Returns null, or the error. */
    public String copyOut(String path, String uri) {
        ContentResolver r = getContentResolver();
        Uri u = Uri.parse(uri);
        OutputStream out = null;
        try {
            try {
                out = r.openOutputStream(u, "wt");
            } catch (Exception e) {
                out = r.openOutputStream(u, "w");   /* some providers know no "wt" */
            }
            if (out == null) return "no output";
            try (InputStream in = new FileInputStream(path)) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
            }
            out.close();
            return null;
        } catch (Exception e) {
            try { if (out != null) out.close(); } catch (Exception e2) { /* already failed */ }
            return e.toString();
        }
    }

    /* A new document `name` in a picked folder; returns its URI. */
    public String createIn(String tree, String name, String mime) {
        try {
            Uri t = Uri.parse(tree);
            Uri parent = DocumentsContract.buildDocumentUriUsingTree(t, DocumentsContract.getTreeDocumentId(t));
            Uri d = DocumentsContract.createDocument(getContentResolver(), parent, mime, name);
            return d == null ? null : d.toString();
        } catch (Exception e) {
            return null;
        }
    }
}
