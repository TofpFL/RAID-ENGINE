package com.godshand.game;

import android.app.NativeActivity;
import android.content.ClipData;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.View;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The game itself is native code (libgodshand.so, started by NativeActivity).
 * This class only hides the system bars and lets the player import mod zips
 * with the system file picker (Freeplay: IMPORT ZIP).
 */
public class GameActivity extends NativeActivity {
    private static final int PICK_ZIPS = 4201;

    /** Read by the game: 0 idle, 1 picking, 2 copying, 3 done, 4 nothing imported. */
    public static volatile int importState = 0;
    /** Zips copied into mods/ by the last import. */
    public static volatile int importCount = 0;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        hideSystemBars();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    @SuppressWarnings("deprecation")
    private void hideSystemBars() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
    }

    /** Called from the game thread. */
    public void pickModZips() {
        importState = 1;
        importCount = 0;
        runOnUiThread(new Picker(this));
    }

    // Named classes rather than anonymous ones: some d8 versions cannot read
    // the constructors newer javac versions write for anonymous classes.
    private static final class Picker implements Runnable {
        private final GameActivity activity;

        Picker(GameActivity activity) {
            this.activity = activity;
        }

        @Override
        public void run() {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            try {
                activity.startActivityForResult(i, PICK_ZIPS);
            } catch (Exception e) {
                importState = 4;
            }
        }
    }

    private static final class Copier extends Thread {
        private final GameActivity activity;
        private final List<Uri> uris;

        Copier(GameActivity activity, List<Uri> uris) {
            this.activity = activity;
            this.uris = uris;
        }

        @Override
        public void run() {
            File base = activity.getExternalFilesDir(null);
            if (base == null) base = activity.getFilesDir();
            File dir = new File(base, "mods");
            dir.mkdirs();
            int n = 0;
            for (Uri u : uris) {
                if (activity.copyZip(u, dir)) ++n;
            }
            importCount = n;
            importState = n > 0 ? 3 : 4;
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_ZIPS) return;
        List<Uri> uris = new ArrayList<>();
        if (resultCode == RESULT_OK && data != null) {
            ClipData clip = data.getClipData();
            if (clip != null) {
                for (int k = 0; k < clip.getItemCount(); ++k) uris.add(clip.getItemAt(k).getUri());
            } else if (data.getData() != null) {
                uris.add(data.getData());
            }
        }
        if (uris.isEmpty()) {
            importState = 4;
            return;
        }
        importState = 2;
        new Copier(this, uris).start();
    }

    private boolean copyZip(Uri uri, File dir) {
        String name = displayName(uri);
        if (name == null || name.isEmpty()) return false;
        name = name.replaceAll("[\\\\/:*?\"<>|]", "_");
        if (!name.toLowerCase(Locale.ROOT).endsWith(".zip")) return false;  // mods come as zips
        File part = new File(dir, name + ".part");
        File out = new File(dir, name);
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream os = new FileOutputStream(part)) {
            if (in == null) return false;
            byte[] buf = new byte[1 << 16];
            for (int r; (r = in.read(buf)) > 0; ) os.write(buf, 0, r);
        } catch (Exception e) {
            part.delete();
            return false;
        }
        out.delete();
        return part.renameTo(out);
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception e) {
            // fall through
        }
        return uri.getLastPathSegment();
    }
}
