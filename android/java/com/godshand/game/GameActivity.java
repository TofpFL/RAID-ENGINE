package com.godshand.game;

import android.app.NativeActivity;
import android.content.ClipData;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.view.View;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * The game itself is native code (libgodshand.so, started by NativeActivity).
 * This class only hides the system bars, lets the player import mod zips
 * with the system file picker (Freeplay: IMPORT ZIP) and copies the pieces of
 * the Psych Engine interface out of a folder they choose (Options).
 */
public class GameActivity extends NativeActivity {
    private static final int PICK_ZIPS = 4201;
    private static final int PICK_FOLDER = 4202;

    /** Files the Psych Engine interface uses (lower case), wherever they are in the folder. */
    private static final Set<String> PSYCH_FILES = new HashSet<>(Arrays.asList(
            "note_assets.png", "note_assets.xml", "sick.png", "good.png", "bad.png", "shit.png",
            "num0.png", "num1.png", "num2.png", "num3.png", "num4.png", "num5.png", "num6.png", "num7.png", "num8.png",
            "num9.png", "ready.png", "set.png", "go.png", "healthbar.png", "vcr.ttf", "scrollmenu.ogg", "confirmmenu.ogg",
            "cancelmenu.ogg", "intro3.ogg", "intro2.ogg", "intro1.ogg", "introgo.ogg", "missnote1.ogg", "hitsound.ogg",
            "fnf_loss_sfx.ogg", "freakymenu.ogg"));
    /** Folders with nothing of the interface in them (and often very large). */
    private static final Set<String> SKIP_DIRS = new HashSet<>(Arrays.asList(
            "mods", "songs", "videos", "characters", "stages", "weeks", "data", "shaders", "scripts", "custom_events",
            "custom_notetypes", "week1", "week2", "week3", "week4", "week5", "week6", "week7", "manifest", "lime", "flixel",
            "obj", "export", ".git", "dialogue", "menucharacters", "menubackgrounds", "storymenu", "credits", "icons"));

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

    /** Called from the game thread: Options, Psych Engine folder. */
    public void pickPsychFolder() {
        importState = 1;
        importCount = 0;
        runOnUiThread(new FolderPicker(this));
    }

    private static final class FolderPicker implements Runnable {
        private final GameActivity activity;

        FolderPicker(GameActivity activity) {
            this.activity = activity;
        }

        @Override
        public void run() {
            try {
                activity.startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER);
            } catch (Exception e) {
                importState = 4;
            }
        }
    }

    /** Copies the interface files out of the chosen folder into psych-ui/, keeping their paths. */
    private static final class FolderCopier extends Thread {
        private final GameActivity activity;
        private final Uri tree;
        private int copied = 0, visited = 0;

        FolderCopier(GameActivity activity, Uri tree) {
            this.activity = activity;
            this.tree = tree;
        }

        @Override
        public void run() {
            File base = activity.getExternalFilesDir(null);
            if (base == null) base = activity.getFilesDir();
            File out = new File(base, "psych-ui");
            deleteTree(out);
            out.mkdirs();
            try {
                walk(DocumentsContract.getTreeDocumentId(tree), out, 0);
            } catch (Exception e) {
                // whatever was copied stays
            }
            importCount = copied;
            importState = copied > 0 ? 3 : 4;
        }

        private void walk(String docId, File dir, int depth) {
            if (depth > 8 || visited > 40000) return;
            Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
            String[] cols = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                    DocumentsContract.Document.COLUMN_MIME_TYPE};
            try (Cursor c = activity.getContentResolver().query(children, cols, null, null, null)) {
                if (c == null) return;
                while (c.moveToNext()) {
                    ++visited;
                    String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                    if (name == null) continue;
                    String lower = name.toLowerCase(Locale.ROOT);
                    if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                        if (!SKIP_DIRS.contains(lower)) walk(id, new File(dir, name.replaceAll("[\\\\/:*?\"<>|]", "_")), depth + 1);
                    } else if (PSYCH_FILES.contains(lower)) {
                        dir.mkdirs();
                        if (activity.copyTo(DocumentsContract.buildDocumentUriUsingTree(tree, id), new File(dir, name))) ++copied;
                    }
                }
            } catch (Exception e) {
                // an unreadable folder is skipped
            }
        }

        private static void deleteTree(File f) {
            File[] kids = f.listFiles();
            if (kids != null) for (File k : kids) deleteTree(k);
            f.delete();
        }
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
        if (requestCode == PICK_FOLDER) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) {
                importState = 2;
                new FolderCopier(this, data.getData()).start();
            } else {
                importState = 4;
            }
            return;
        }
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

    private boolean copyTo(Uri uri, File out) {
        File part = new File(out.getPath() + ".part");
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
