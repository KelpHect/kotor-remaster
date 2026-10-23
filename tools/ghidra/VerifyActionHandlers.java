// Independent verification of the saved handler-definition pass.
// Opens the copy read-only; checks dispatcher, every handler's function/comment, and
// decompiles a deterministic sample. Writes verification.json.
// @category KotOR

import java.io.FileReader;
import java.io.PrintWriter;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.TreeMap;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.CommentType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.FlowType;

public class VerifyActionHandlers extends GhidraScript {

    private static String esc(String s) {
        if (s == null) {
            return "";
        }
        return s.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", "\\n")
                .replace("\r", "\\r").replace("\t", "\\t");
    }

    private static String sha256(String text) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        byte[] d = md.digest(text.getBytes("UTF-8"));
        StringBuilder b = new StringBuilder();
        for (byte x : d) {
            b.append(String.format("%02x", x));
        }
        return b.toString();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("usage: VerifyActionHandlers.java <action_table.json> <verification.json>");
            return;
        }
        JsonObject root = JsonParser.parseReader(new FileReader(args[0])).getAsJsonObject();
        JsonArray actions = root.getAsJsonArray("actions");
        TreeMap<Long, List<JsonObject>> targets = new TreeMap<>();
        for (JsonElement e : actions) {
            JsonObject a = e.getAsJsonObject();
            String h = a.get("handler").getAsString();
            long addr = Long.parseLong(h.substring(h.indexOf('_') + 1), 16);
            targets.computeIfAbsent(addr, k -> new ArrayList<>()).add(a);
        }
        long daddr = Long.parseLong(root.getAsJsonObject("dispatch").get("entry").getAsString(), 16);

        int labTotal = 0;
        int labFunction = 0;
        int labNameOk = 0;
        int labCommentOk = 0;
        int labBodyOk = 0;
        int labTerminalOk = 0;
        int funTotal = 0;
        int funPresent = 0;
        List<String> problems = new ArrayList<>();
        List<Long> labAddrs = new ArrayList<>();
        List<Long> funAddrs = new ArrayList<>();

        for (java.util.Map.Entry<Long, List<JsonObject>> e : targets.entrySet()) {
            long a = e.getKey();
            String label = e.getValue().get(0).get("handler").getAsString();
            boolean isLab = label.startsWith("LAB_");
            if (isLab) {
                labTotal++;
                labAddrs.add(a);
            } else {
                funTotal++;
                funAddrs.add(a);
            }
            Address addr = toAddr(a);
            Function f = currentProgram.getFunctionManager().getFunctionAt(addr);
            if (f == null) {
                if (isLab) {
                    problems.add("missing function " + label);
                }
                continue;
            }
            if (isLab) {
                labFunction++;
                int minId = Integer.MAX_VALUE;
                for (JsonObject o : e.getValue()) {
                    minId = Math.min(minId, o.get("id").getAsInt());
                }
                if (f.getName().startsWith("ACT_" + minId + "_")) {
                    labNameOk++;
                } else {
                    problems.add("unexpected name at " + label + ": " + f.getName());
                }
                String comment = currentProgram.getListing().getComment(CommentType.PLATE, addr);
                boolean all = comment != null;
                if (all) {
                    for (JsonObject o : e.getValue()) {
                        String needle = o.get("id").getAsInt() + " " + o.get("name").getAsString();
                        if (!comment.contains(needle)) {
                            all = false;
                            break;
                        }
                    }
                }
                if (all) {
                    labCommentOk++;
                } else {
                    problems.add("comment missing action ids at " + label);
                }
                int ic = 0;
                Instruction lastIns = null;
                ghidra.program.model.listing.InstructionIterator it =
                        currentProgram.getListing().getInstructions(f.getBody(), true);
                while (it.hasNext()) {
                    lastIns = it.next();
                    ic++;
                }
                if (ic > 0 && f.getBody().getNumAddresses() > 0) {
                    labBodyOk++;
                } else {
                    problems.add("empty body at " + label);
                }
                if (lastIns != null) {
                    FlowType ft = lastIns.getFlowType();
                    if (ft.isTerminal() || (ft.isJump() && !ft.isConditional())) {
                        labTerminalOk++;
                    } else {
                        problems.add("non-terminal last instruction at " + label + ": "
                                + lastIns);
                    }
                } else {
                    problems.add("no last instruction at " + label);
                }
            } else {
                funPresent++;
            }
        }

        Address da = toAddr(daddr);
        Function df = currentProgram.getFunctionManager().getFunctionAt(da);
        boolean dispatcherOk = df != null && "NWScript_DispatchAction".equals(df.getName());
        String dispatcherComment = df == null ? null
                : currentProgram.getListing().getComment(CommentType.PLATE, da);

        // Deterministic sample across the LAB list (sorted by address).
        DecompInterface di = new DecompInterface();
        di.toggleCCode(true);
        di.toggleSyntaxTree(false);
        di.setSimplificationStyle("decompile");
        boolean decompOpen = di.openProgram(currentProgram);
        StringBuilder sample = new StringBuilder();
        int samples = 0;
        int sampleDecompiled = 0;
        int step = Math.max(1, labAddrs.size() / 15);
        int prologueCount = 0;
        for (int i = 0; i < labAddrs.size() && samples < 15; i += step) {
            long a = labAddrs.get(i);
            Address addr = toAddr(a);
            Function f = currentProgram.getFunctionManager().getFunctionAt(addr);
            if (f == null) {
                continue;
            }
            Instruction ins = currentProgram.getListing().getInstructionAt(addr);
            StringBuilder dis = new StringBuilder();
            Instruction x = ins;
            for (int k = 0; k < 5 && x != null; k++) {
                if (k > 0) {
                    dis.append(" ; ");
                }
                dis.append(x.toString());
                x = x.getNext();
            }
            if (ins != null && !ins.getMnemonicString().equalsIgnoreCase("INT3")) {
                prologueCount++;
            }
            String code = "";
            String err = null;
            if (decompOpen) {
                DecompileResults res = di.decompileFunction(f, 60, monitor);
                if (res != null && res.decompileCompleted() && res.getDecompiledFunction() != null) {
                    code = res.getDecompiledFunction().getC();
                } else {
                    err = res == null ? "null result" : res.getErrorMessage();
                }
            } else {
                err = "decompiler open failed";
            }
            boolean nonEmpty = code != null && !code.trim().isEmpty();
            if (nonEmpty) {
                sampleDecompiled++;
            }
            if (samples > 0) {
                sample.append(",\n");
            }
            sample.append("{\"entry\":\"0x").append(addr).append("\"")
                    .append(",\"name\":\"").append(esc(f.getName())).append("\"")
                    .append(",\"instructions\":").append(count(f))
                    .append(",\"size\":").append(f.getBody().getNumAddresses())
                    .append(",\"disassembly\":\"").append(esc(dis.toString())).append("\"")
                    .append(",\"pseudocode_nonempty\":").append(nonEmpty)
                    .append(",\"pseudocode_length\":").append(nonEmpty ? code.length() : 0)
                    .append(",\"pseudocode_sha256\":\"")
                    .append(nonEmpty ? sha256(code) : "").append("\"");
            if (err != null) {
                sample.append(",\"pseudocode_error\":\"").append(esc(err)).append("\"");
            }
            sample.append("}");
            samples++;
        }
        if (decompOpen) {
            di.dispose();
        }

        int totalFunctionCount = currentProgram.getFunctionManager().getFunctionCount();
        boolean allOk = labTotal == 440 && labFunction == 440 && labNameOk == 440
                && labCommentOk == 440 && labBodyOk == 440 && labTerminalOk == 440
                && funTotal == 125 && funPresent == 125
                && dispatcherOk && sampleDecompiled >= 10 && problems.isEmpty();

        try (PrintWriter out = new PrintWriter(args[1], "UTF-8")) {
            out.println("{");
            out.println("\"program\":\"" + esc(currentProgram.getName()) + "\",");
            out.println("\"program_sha256\":\""
                    + currentProgram.getExecutableSHA256() + "\",");
            out.println("\"function_count\":" + totalFunctionCount + ",");
            out.println("\"dispatcher\":{\"entry\":\"0x" + da + "\",\"name\":\""
                    + esc(df == null ? "" : df.getName()) + "\",\"present\":" + (df != null)
                    + ",\"named_correctly\":" + dispatcherOk + ",\"plate_comment\":\""
                    + esc(dispatcherComment == null ? "" : dispatcherComment.replace("\n", " | "))
                    + "\"},");
            out.println("\"counts\":{\"lab_total\":" + labTotal + ",\"lab_function_at_entry\":"
                    + labFunction + ",\"lab_name_ok\":" + labNameOk + ",\"lab_comment_ok\":" + labCommentOk
                    + ",\"lab_body_ok\":" + labBodyOk + ",\"lab_terminal_ok\":" + labTerminalOk
                    + ",\"fun_total\":" + funTotal
                    + ",\"fun_present\":" + funPresent + ",\"samples\":" + samples
                    + ",\"sample_decompiled\":" + sampleDecompiled + "},");
            out.println("\"all_checks_pass\":" + allOk + ",");
            out.println("\"problems\":[");
            for (int i = 0; i < problems.size(); i++) {
                out.println((i > 0 ? "," : "") + "\"" + esc(problems.get(i)) + "\"");
            }
            out.println("],");
            out.println("\"samples\":[");
            out.println(sample.toString());
            out.println("]");
            out.println("}");
        }
        println("VERIFY lab=" + labTotal + " labFn=" + labFunction + " name=" + labNameOk
                + " comment=" + labCommentOk + " body=" + labBodyOk + " terminal=" + labTerminalOk
                + " fun=" + funPresent + "/" + funTotal
                + " dispatcher=" + dispatcherOk + " samples=" + samples + " sampleDecompiled="
                + sampleDecompiled + " allOk=" + allOk + " problems=" + problems.size());
    }

    private int count(Function f) {
        int n = 0;
        ghidra.program.model.listing.InstructionIterator it =
                currentProgram.getListing().getInstructions(f.getBody(), true);
        while (it.hasNext()) {
            it.next();
            n++;
        }
        return n;
    }
}
