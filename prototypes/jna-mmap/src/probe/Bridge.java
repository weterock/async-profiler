package probe;

/** Bootstrap-visible helper. No dependency on JNA or ASM. */
public final class Bridge {
    private Bridge() {}
    public static void load(String library, String log) {
        System.load(library);
        initialize(log);
    }
    private static native void initialize(String log);
    public static native long redirect(long address);
    public static native void enabled(boolean value);
}
