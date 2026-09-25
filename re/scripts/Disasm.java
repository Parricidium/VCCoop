// Desassemble une plage : arguments <fichier de sortie> <debut> <nombre d'instructions> [<debut> <n>]...
// Desassemble a la volee si Ghidra n'a pas encore d'instructions a cet endroit.
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;

public class Disasm extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(args[0], "UTF-8");
        Listing listing = currentProgram.getListing();
        for (int i = 1; i + 1 < args.length; i += 2) {
            Address a = toAddr(Long.decode(args[i]));
            int n = Integer.parseInt(args[i + 1]);
            if (listing.getInstructionAt(a) == null) new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
            Function f = getFunctionContaining(a);
            out.println("//==== " + a + (f != null ? "  (dans " + f.getName() + " @ " + f.getEntryPoint() + ")" : ""));
            Instruction ins = listing.getInstructionAt(a);
            for (int k = 0; k < n && ins != null; k++) {
                out.println(ins.getAddress() + "  " + ins);
                Instruction next = ins.getNext();
                if (next == null) {
                    Address na = ins.getMaxAddress().add(1);
                    new DisassembleCommand(na, null, true).applyTo(currentProgram, monitor);
                    next = listing.getInstructionAt(na);
                }
                ins = next;
            }
        }
        out.close();
    }
}
