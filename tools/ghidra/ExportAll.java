// Headless: dump decompiled C and disassembly of every function.
// Usage: analyzeHeadless <proj> <name> -import X.EXE -postScript ExportAll.java <outdir>
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;

public class ExportAll extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outDir = getScriptArgs()[0];
        String name = currentProgram.getName();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter c = new PrintWriter(new File(outDir, name + ".c"));
             PrintWriter a = new PrintWriter(new File(outDir, name + ".asm"))) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                DecompileResults r = di.decompileFunction(f, 60, monitor);
                c.println("// ==== " + f.getName() + " @ " + f.getEntryPoint());
                c.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed");
                a.println("; ==== " + f.getName() + " @ " + f.getEntryPoint());
                for (Instruction i : currentProgram.getListing().getInstructions(f.getBody(), true))
                    a.println(i.getAddress() + "  " + i);
            }
        }
    }
}
