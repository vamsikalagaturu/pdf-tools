package io.github.vamsikalagaturu.pdftools;

import android.Manifest;
import android.app.NativeActivity;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/* The native window plus what only Java can reach: the system document picker (Storage Access Framework),
 * content:// I/O and the storage permission. Called from the native thread (android_main.c and
 * file_dialog_android.c); every method takes and returns strings, null meaning "no" or "nothing". */
public class PdfToolsActivity extends NativeActivity {
    private static final int REQ_PICK = 1, REQ_STORAGE = 2;
    private final Object lock = new Object();
    private String pickResult;        /* null while pending, "" when cancelled, else URIs separated by '\n' */
    private Intent waitingPick;       /* a pick deferred until the storage request returns */
    private boolean askedStorage;     /* asked in this run: a file dialog asks at most once per run */
    private boolean checkedFirstStart;

    /* NativeActivity drops intents delivered to a running instance (launchMode singleTask); keeping the latest
     * lets the native side see a PDF opened from another app while the window is up. */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
    }

    /* Ask for storage once, at the first start that reaches the window; later only when a file dialog opens. */
    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || checkedFirstStart) return;
        checkedFirstStart = true;
        SharedPreferences prefs = getPreferences(MODE_PRIVATE);
        if (prefs.getBoolean("storage_asked", false) || storageGranted()) return;
        prefs.edit().putBoolean("storage_asked", true).apply();
        askedStorage = true;
        requestStorage();
    }

    private boolean storageGranted() {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
            && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    private void requestStorage() {
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                startActivityForResult(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())), REQ_STORAGE);
            } catch (Exception e) {
                try {
                    startActivityForResult(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION), REQ_STORAGE);
                } catch (Exception e2) {
                    storageAnswered();
                }
            }
        } else {
            requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE,
                Manifest.permission.WRITE_EXTERNAL_STORAGE}, REQ_STORAGE);
        }
    }

    private void storageAnswered() {
        Intent pick = waitingPick;
        waitingPick = null;
        if (pick != null) startPick(pick);
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] permissions, int[] results) {
        if (code == REQ_STORAGE) storageAnswered();
    }

    public String hasStorage() { return storageGranted() ? "1" : null; }

    /* The shared Download folder when storage access is granted. */
    public String sharedFolder() {
        if (!storageGranted()) return null;
        File d = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        if (!d.isDirectory()) d = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS);
        return d.isDirectory() ? d.getAbsolutePath() : null;
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
        runOnUiThread(() -> {
            if (!storageGranted() && !askedStorage) {
                askedStorage = true;
                waitingPick = intent;
                requestStorage();
            } else startPick(intent);
        });
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
        if (code == REQ_STORAGE) {
            storageAnswered();
            return;
        }
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

    /* The file behind a document (or a picked folder) on shared storage, when storage access lets us use it. */
    public String realPath(String uri) {
        if (!storageGranted()) return null;
        Uri u = Uri.parse(uri);
        String path = null;
        try {
            boolean doc = DocumentsContract.isDocumentUri(this, u);
            if ("com.android.externalstorage.documents".equals(u.getAuthority())) {
                String id = doc ? DocumentsContract.getDocumentId(u) : DocumentsContract.getTreeDocumentId(u);
                int colon = id.indexOf(':');
                String volume = id.substring(0, colon), rel = id.substring(colon + 1);
                File root = volume.equals("primary") ? Environment.getExternalStorageDirectory() : new File("/storage/" + volume);
                path = rel.isEmpty() ? root.getPath() : new File(root, rel).getPath();
            } else if (doc && DocumentsContract.getDocumentId(u).startsWith("raw:")) {
                path = DocumentsContract.getDocumentId(u).substring(4);
            } else if (doc || "file".equals(u.getScheme()) || "media".equals(u.getAuthority())) {
                if ("file".equals(u.getScheme())) path = u.getPath();
                else try (Cursor c = getContentResolver().query(u, new String[] {"_data"}, null, null, null)) {
                    if (c != null && c.moveToFirst()) path = c.getString(0);
                }
            }
        } catch (Exception e) {
            path = null;
        }
        return path != null && new File(path).exists() ? path : null;
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
