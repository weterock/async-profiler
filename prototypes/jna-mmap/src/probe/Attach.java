package probe;
import com.sun.tools.attach.VirtualMachine;
import java.io.File;
public final class Attach {
    public static void main(String[] args) throws Exception {
        if (args.length < 2 || args.length > 3) {
            throw new IllegalArgumentException("Attach <pid> <absolute-build-directory> [stop]");
        }
        File dir = new File(args[1]).getCanonicalFile();
        String option = args.length == 3 ? args[2] : dir.getPath();
        if (args.length == 3 && !option.equals("stop")) throw new IllegalArgumentException("Expected stop");
        VirtualMachine vm = VirtualMachine.attach(args[0]);
        try { vm.loadAgent(new File(dir, "agent.jar").getPath(), option); }
        finally { vm.detach(); }
        System.out.println("Attach completed: " + (option.equals("stop") ? "stop" : "start"));
    }
}
