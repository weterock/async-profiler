package probe;

import java.io.File;
import java.lang.instrument.*;
import java.lang.reflect.Method;
import java.security.ProtectionDomain;
import java.util.*;
import java.util.jar.JarFile;
import org.objectweb.asm.*;

public final class Agent {
    private static Class<?> bridge;
    private static Transformer transformer;
    private static JarFile helperJar;
    private static Instrumentation installedWith;

    public static synchronized void agentmain(String options, Instrumentation inst) throws Exception {
        if ("stop".equals(options)) {
            if (transformer == null) throw new IllegalStateException("Not active");
            bridge.getMethod("enabled", boolean.class).invoke(null, false);
            if (!installedWith.removeTransformer(transformer))
                throw new IllegalStateException("Could not remove transformer");
            transformer = null;
            retransform(installedWith);
            System.err.println("[probe] STOPPED; helper/native library remain loaded");
            return;
        }
        if (transformer != null) throw new IllegalStateException("Already active; stop first");
        if (!inst.isRetransformClassesSupported()) throw new IllegalStateException("Retransformation unavailable");
        File dir = new File(options).getCanonicalFile();
        if (bridge == null) {
            helperJar = new JarFile(new File(dir, "bridge.jar"));
            inst.appendToBootstrapClassLoaderSearch(helperJar);
            bridge = Class.forName("probe.Bridge", true, null);
            String lib = System.mapLibraryName("mmapprobe");
            bridge.getMethod("load", String.class, String.class).invoke(null,
                new File(dir, lib).getPath(), new File(dir, "events.tsv").getPath());
        }
        Transformer candidate = new Transformer();
        inst.addTransformer(candidate, true);
        try {
            int count = retransform(inst);
            if (candidate.failure != null) throw new IllegalStateException(candidate.failure);
            bridge.getMethod("enabled", boolean.class).invoke(null, true);
            transformer = candidate;
            installedWith = inst;
            System.err.println("[probe] ACTIVE; loaded Function classes=" + count
                + "; log=" + new File(dir, "events.tsv"));
        } catch (Throwable error) {
            inst.removeTransformer(candidate);
            retransform(inst);
            throw new IllegalStateException("Activation failed", error);
        }
    }

    private static int retransform(Instrumentation inst) throws Exception {
        List<Class<?>> targets = new ArrayList<>();
        for (Class<?> c : inst.getAllLoadedClasses()) {
            if (c.getName().equals("com.sun.jna.Function")) {
                if (!inst.isModifiableClass(c)) throw new IllegalStateException("Function is not modifiable");
                targets.add(c);
            }
        }
        if (!targets.isEmpty()) inst.retransformClasses(targets.toArray(new Class<?>[0]));
        return targets.size();
    }

    static final class Transformer implements ClassFileTransformer {
        volatile String failure;
        public byte[] transform(ClassLoader loader, String name, Class<?> redef,
                                ProtectionDomain domain, byte[] bytes) {
            if (!"com/sun/jna/Function".equals(name)) return null;
            try {
                ClassReader reader = new ClassReader(bytes);
                ClassWriter writer = new ClassWriter(reader, 0);
                final int[] sites = {0};
                reader.accept(new ClassVisitor(Opcodes.ASM9, writer) {
                    public MethodVisitor visitMethod(int access, String method, String desc,
                                                     String signature, String[] exceptions) {
                        MethodVisitor next = super.visitMethod(access, method, desc, signature, exceptions);
                        // Exact JNA 5.8.0 conversion/dispatch method; do not mutate peer or the cache.
                        if (!method.equals("invoke") || !desc.equals(
                            "([Ljava/lang/Object;Ljava/lang/Class;ZI)Ljava/lang/Object;")) return next;
                        return new MethodVisitor(Opcodes.ASM9, next) {
                            public void visitFieldInsn(int opcode, String owner, String field, String type) {
                                super.visitFieldInsn(opcode, owner, field, type);
                                if (opcode == Opcodes.GETFIELD && field.equals("peer") && type.equals("J")
                                    && (owner.equals("com/sun/jna/Function") || owner.equals("com/sun/jna/Pointer"))) {
                                    // long -> long: no new locals, no changed stack height or frame types.
                                    super.visitMethodInsn(Opcodes.INVOKESTATIC, "probe/Bridge", "redirect", "(J)J", false);
                                    sites[0]++;
                                }
                            }
                        };
                    }
                }, 0);
                if (sites[0] == 0) throw new IllegalStateException("Unsupported JNA bytecode: no dispatch peer reads");
                System.err.println("[probe] transformed Function; peer-read sites=" + sites[0]);
                return writer.toByteArray();
            } catch (Throwable e) {
                failure = e.toString();
                System.err.println("[probe] TRANSFORM FAILED: " + e);
                return null;
            }
        }
    }
}
