// Dumps the raw planar BitmapImage after decoding the first frame.
import ch.randelshofer.media.seq.*;
import ch.randelshofer.gui.image.BitmapImage;
import java.io.*;

public class SeqDumpPlanar {
    public static void main(String[] args) throws Exception {
        SEQMovieTrack track = new SEQMovieTrack();
        try (FileInputStream fis = new FileInputStream(args[0])) {
            new SEQDecoder(fis).produce(track, false);
        }
        BitmapImage bmp = new BitmapImage(track.getWidth(), track.getHeight(),
                                          track.getNbPlanes(), null);
        track.getFrame(0).decode(bmp, track);
        byte[] b = bmp.getBitmap();
        int frame = 0;
        if (args.length > 2) frame = Integer.parseInt(args[2]);
        for (int i = 1; i <= frame; i++) {
            track.getFrame(i).decode(bmp, track);
        }
        System.err.printf("scanline_stride=%d bitplane_stride=%d depth=%d size=%d%n",
                bmp.getScanlineStride(), bmp.getBitplaneStride(), bmp.getDepth(), b.length);
        try (FileOutputStream fos = new FileOutputStream(args[1])) {
            fos.write(b);
        }
    }
}
