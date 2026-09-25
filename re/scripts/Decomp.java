// Decompile des fonctions donnees par adresse (et, avec "xref:ADDR", toutes celles qui referencent ADDR).
// Arguments : <fichier de sortie> <adresse|xref:adresse>...
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import java.io.PrintWriter;
import java.util.*;

public class Decomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outPath = args[0];
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);

        Set<Function> fns = new LinkedHashSet<>();
        for (int i = 1; i < args.length; i++) {
            String a = args[i];
            if (a.startsWith("xref:")) {
                Address t = toAddr(Long.decode(a.substring(5)));
                ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(t);
                while (it.hasNext()) {
                    Reference r = it.next();
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f != null) fns.add(f);
                }
            } else {
                Address t = toAddr(Long.decode(a));
                Function f = getFunctionContaining(t);
                if (f == null) f = createFunction(t, null);
                if (f != null) fns.add(f);
            }
        }

        PrintWriter out = new PrintWriter(outPath, "UTF-8");
        for (Function f : fns) {
            out.println("//==== " + f.getName() + " @ " + f.getEntryPoint());
            DecompileResults res = d.decompileFunction(f, 120, monitor);
            if (res != null && res.decompileCompleted()) out.println(res.getDecompiledFunction().getC());
            else out.println("// echec decompilation");
        }
        out.close();
        println("Ecrit : " + outPath + " (" + fns.size() + " fonctions)");
    }
}
