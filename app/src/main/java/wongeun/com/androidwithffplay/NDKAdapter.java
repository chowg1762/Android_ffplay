package wongeun.com.androidwithffplay;

public class NDKAdapter {
    static {
        System.loadLibrary("VideoPlayer");
    }

    public static native void setDataSource(String uri);
    public static native int play(Object surface);
    public static native void setPlayingState(int isPlaying);

    public NDKAdapter(){

    }
}
