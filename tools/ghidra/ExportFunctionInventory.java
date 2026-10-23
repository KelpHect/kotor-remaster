// Exports every function in the current Ghidra program to JSONL.
// Usage (headless):
//   analyzeHeadless <proj> <name> -process swkotor.exe -noanalysis \
//     -scriptPath tools/ghidra -postScript ExportFunctionInventory.java <out.jsonl> [--decompile]
// @category KotOR

import java.io.FileWriter;
import java.io.PrintWriter;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

public class ExportFunctionInventory extends GhidraScript {

    private static String esc(String s) {
        if (s == null) {
            return "";
        }
        StringBuilder b = new StringBuilder(s.length() + 16);
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"': b.append("\\\""); break;
                case '\\': b.append("\\\\"); break;
                case '\n': b.append("\\n"); break;
                case '\r': b.append("\\r"); break;
                case '\t': b.append("\\t"); break;
                default:
                    if (c < 0x20) {
                        b.append(String.format("\\u%04x", (int) c));
                    } else {
                        b.append(c);
                    }
            }
        }
        return b.toString();
    }

    private static String sha256(String text) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        byte[] digest = md.digest(text.getBytes("UTF-8"));
        StringBuilder b = new StringBuilder();
        for (byte x : digest) {
            b.append(String.format("%02x", x));
        }
        return b.toString();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("usage: ExportFunctionInventory.java <out.jsonl> [--decompile]");
            return;
        }
        String outPath = args[0];
        boolean decompile = args.length > 1 && args[1].equals("--decompile");

        DecompInterface decompiler = null;
        if (decompile) {
            decompiler = new DecompInterface();
            decompiler.toggleCCode(true);
            decompiler.toggleSyntaxTree(false);
            decompiler.setSimplificationStyle("decompile");
            if (!decompiler.openProgram(currentProgram)) {
                printerr("decompiler failed to open program: " + decompiler.getLastMessage());
                decompiler = null;
            }
        }

        int total = 0;
        int external = 0;
        int thunks = 0;
        int decompiled = 0;
        List<String> lines = new ArrayList<>();
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        try (PrintWriter out = new PrintWriter(new FileWriter(outPath))) {
            while (it.hasNext() && !monitor.isCancelled()) {
                Function f = it.next();
                total++;
                Address entry = f.getEntryPoint();
                long size = f.getBody().getNumAddresses();
                int instructions = 0;
                InstructionIterator ii = currentProgram.getListing().getInstructions(f.getBody(), true);
                while (ii.hasNext()) {
                    ii.next();
                    instructions++;
                }
                if (f.isExternal()) {
                    external++;
                }
                if (f.isThunk()) {
                    thunks++;
                }
                StringBuilder b = new StringBuilder(512);
                b.append("{\"entry\":\"").append(entry).append("\"");
                b.append(",\"name\":\"").append(esc(f.getName())).append("\"");
                b.append(",\"namespace\":\"").append(esc(f.getParentNamespace() == null ? "" : f.getParentNamespace().getName(true))).append("\"");
                b.append(",\"size\":").append(size);
                b.append(",\"instructions\":").append(instructions);
                b.append(",\"external\":").append(f.isExternal());
                b.append(",\"thunk\":").append(f.isThunk());
                b.append(",\"calling_convention\":\"").append(esc(f.getCallingConventionName())).append("\"");
                b.append(",\"parameter_count\":").append(f.getParameterCount());
                b.append(",\"return_type\":\"").append(esc(f.getReturnType() == null ? "" : f.getReturnType().getName())).append("\"");
                b.append(",\"body_sha256\":\"").append(sha256(entry + ":" + size + ":" + f.getName())).append("\"");
                if (decompiler != null) {
                    try {
                        DecompileResults res = decompiler.decompileFunction(f, 90, monitor);
                        if (res != null && res.decompileCompleted() && res.getDecompiledFunction() != null) {
                            String c = res.getDecompiledFunction().getC();
                            b.append(",\"pseudocode_sha256\":\"").append(sha256(c)).append("\"");
                            b.append(",\"pseudocode\":\"").append(esc(c)).append("\"");
                            decompiled++;
                        } else {
                            b.append(",\"pseudocode_error\":\"").append(esc(res == null ? "null result" : res.getErrorMessage())).append("\"");
                        }
                    } catch (Exception e) {
                        b.append(",\"pseudocode_error\":\"").append(esc(String.valueOf(e))).append("\"");
                    }
                }
                b.append("}");
                out.println(b.toString());
            }
        } finally {
            if (decompiler != null) {
                decompiler.dispose();
            }
        }
        println("functions=" + total + " external=" + external + " thunks=" + thunks
                + " decompiled=" + decompiled + " out=" + outPath);
    }
}
