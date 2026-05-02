import ch.randelshofer.media.seq.*;
import ch.randelshofer.gui.image.BitmapImage;
import java.awt.image.IndexColorModel;
import java.io.*;

public class SeqDumpChunky {
    public static void main(String[] args) throws Exception {
        SEQMovieTrack track = new SEQMovieTrack();
        try (FileInputStream fis = new FileInputStream(args[0])) {
            new SEQDecoder(fis).produce(track, false);
        }
        BitmapImage bmp = new BitmapImage(track.getWidth(), track.getHeight(),
                                          track.getNbPlanes(), null);
        SEQFrame f = track.getFrame(0);
        f.decode(bmp, track);
        bmp.setPlanarColorModel(f.getColorModel());
        bmp.convertToChunky();
        byte[] indexed = bmp.getBytePixels();
        // Dump first 32 chunky pixels of row 1 (so we can see pixel (8,1))
        int W = bmp.getWidth();
        System.err.printf("row 1 chunky pixels: ");
        for (int x = 0; x < 32; x++) {
            System.err.printf("%2d ", indexed[1 * W + x] & 0xff);
        }
        System.err.println();
        // Plus dump palette entries 12-15
        IndexColorModel cm = (IndexColorModel) f.getColorModel();
        byte[] r = new byte[cm.getMapSize()], g = new byte[cm.getMapSize()], bb = new byte[cm.getMapSize()];
        cm.getReds(r); cm.getGreens(g); cm.getBlues(bb);
        for (int i = 0; i < 16; i++) {
            System.err.printf("palette[%2d] = (%02x, %02x, %02x)%n",
                i, r[i] & 0xff, g[i] & 0xff, bb[i] & 0xff);
        }
    }
}
