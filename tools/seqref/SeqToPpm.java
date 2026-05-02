// Reference SEQ → PPM dumper using Werner Randelshofer's seqconverter library
// (the same library that ships in seqconverter.jar). Used by onyx_anim's SEQ
// cross-check to compare against our native decoder.
//
// Build:
//   javac -cp seqconverter.jar SeqToPpm.java
// Run:
//   java -cp seqconverter.jar:. SeqToPpm <input.seq> <output_dir>
//
// Writes frame_NNNN.ppm (P6 RGB) into <output_dir>, frames numbered from 0.

import ch.randelshofer.media.seq.SEQDecoder;
import ch.randelshofer.media.seq.SEQFrame;
import ch.randelshofer.media.seq.SEQMovieTrack;
import ch.randelshofer.gui.image.BitmapImage;

import java.awt.image.IndexColorModel;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.OutputStream;

public class SeqToPpm {

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("Usage: SeqToPpm <input.seq> <output_dir>");
            System.exit(2);
        }
        String inputPath = args[0];
        File outDir = new File(args[1]);
        if (!outDir.isDirectory()) {
            System.err.println("output dir does not exist: " + outDir);
            System.exit(2);
        }

        SEQMovieTrack track = new SEQMovieTrack();
        try (FileInputStream fis = new FileInputStream(inputPath)) {
            SEQDecoder dec = new SEQDecoder(fis);
            dec.produce(track, false);
        }

        final int w = track.getWidth();
        final int h = track.getHeight();
        final int planes = track.getNbPlanes();
        BitmapImage bmp = new BitmapImage(w, h, planes, null);

        final int n = track.getFrameCount();
        for (int i = 0; i < n; i++) {
            SEQFrame f = track.getFrame(i);
            f.decode(bmp, track);

            bmp.setPlanarColorModel(f.getColorModel());
            bmp.convertToChunky();
            byte[] indexed = bmp.getBytePixels();

            IndexColorModel cm = (IndexColorModel) f.getColorModel();
            int paletteLen = cm.getMapSize();
            byte[] r = new byte[paletteLen];
            byte[] g = new byte[paletteLen];
            byte[] b = new byte[paletteLen];
            cm.getReds(r);
            cm.getGreens(g);
            cm.getBlues(b);

            File out = new File(outDir, String.format("frame_%04d.ppm", i));
            try (OutputStream os = new BufferedOutputStream(new FileOutputStream(out))) {
                String header = "P6\n" + w + " " + h + "\n255\n";
                os.write(header.getBytes("US-ASCII"));
                byte[] row = new byte[w * 3];
                // The chunky `indexed` buffer is width*height bytes (no
                // padding), regardless of the planar bitmap's scanlineStride.
                for (int y = 0; y < h; y++) {
                    int base = y * w;
                    for (int x = 0; x < w; x++) {
                        int idx = indexed[base + x] & 0xff;
                        if (idx >= paletteLen) idx = 0;
                        row[x * 3 + 0] = r[idx];
                        row[x * 3 + 1] = g[idx];
                        row[x * 3 + 2] = b[idx];
                    }
                    os.write(row);
                }
            }
        }
        System.out.println(n + " frames written to " + outDir);
    }
}
