// Audits NWScript action handler targets from artifacts/action_table.json, defines the
// dispatcher, creates justified handler functions, comments shared handlers, and decompiles
// every newly created function.
//
// Usage (headless, on a COPY of the project):
//   analyzeHeadless <projDir> <projName> -process swkotor.exe -noanalysis \
//     -scriptPath tools/ghidra \
//     -postScript DefineActionHandlers.java <action_table.json> <audit.tsv> <new_functions.jsonl> \
//         <audit|apply> [limit]
//
// audit mode still creates functions in memory so overlap/alias dispositions match apply mode;
// run it with -readOnly so nothing is persisted.
// @category KotOR

import java.io.FileReader;
import java.io.PrintWriter;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.CommentType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryAccessException;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.SymbolIterator;

public class DefineActionHandlers extends GhidraScript {

    private static final String DISPATCHER_NAME = "NWScript_DispatchAction";
    private static final long DISPATCH_TABLE = 0x007450e4L;
    private static final int DISPATCH_SLOTS = 772;

    private static class Act {
        int id;
        String name;
        String args;
        String handler;
        String registeredBy;
    }

    private static class Target {
        long addr;
        String label;
        List<Act> acts = new ArrayList<>();
    }

    private static class Row {
        String kind;
        long addr;
        String label;
        String functionEntry = "";
        String functionName = "";
        int primaryId = -1;
        String primaryName = "";
        String actionIds = "";
        String actionNames = "";
        String disposition = "";
        int instructions = -1;
        long size = -1;
        String detail = "";
    }

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

    private static String hex(Address a) {
        return "0x" + a.toString();
    }

    private static String tsv(String s) {
        return s == null ? "" : s.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ');
    }

    private static String sanitize(String s) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < s.length() && b.length() < 40; i++) {
            char c = s.charAt(i);
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                b.append(c);
            } else if (b.length() > 0 && b.charAt(b.length() - 1) != '_') {
                b.append('_');
            }
        }
        while (b.length() > 0 && b.charAt(b.length() - 1) == '_') {
            b.setLength(b.length() - 1);
        }
        return b.length() == 0 ? "handler" : b.toString();
    }

    private static boolean isTerminal(Instruction ins) {
        FlowType ft = ins.getFlowType();
        if (ft.isTerminal()) {
            return true;
        }
        return ft.isJump() && !ft.isConditional();
    }

    private static String prologueKind(byte[] b, int len) {
        if (len >= 3 && (b[0] & 0xff) == 0x55 && (b[1] & 0xff) == 0x8b && (b[2] & 0xff) == 0xec) {
            return "push_ebp_mov_ebp_esp";
        }
        if (len >= 2 && (b[0] & 0xff) == 0x55 && (b[1] & 0xff) == 0x89) {
            return "push_ebp_mov_ebp_esp2";
        }
        if (len >= 2 && (b[0] & 0xff) == 0x8b && (b[1] & 0xff) == 0xff) {
            return "hotpatch_mov_edi_edi";
        }
        if (len >= 2 && (b[0] & 0xff) == 0x83 && (b[1] & 0xff) == 0xec) {
            return "sub_esp_imm8";
        }
        if (len >= 2 && (b[0] & 0xff) == 0x81 && (b[1] & 0xff) == 0xec) {
            return "sub_esp_imm32";
        }
        if (len >= 1 && ((b[0] & 0xff) == 0x53 || (b[0] & 0xff) == 0x56 || (b[0] & 0xff) == 0x57)) {
            return "push_reg";
        }
        if (len >= 1 && (b[0] & 0xff) == 0x51) {
            return "push_ecx_thiscall_local";
        }
        if (len >= 6 && (b[0] & 0xff) == 0x64 && (b[1] & 0xff) == 0xa1) {
            return "seh_fs0";
        }
        return "";
    }

    private Function createAt(Address addr, String name, AddressSetView body) {
        try {
            CreateFunctionCmd cmd = new CreateFunctionCmd(name, addr, body, SourceType.USER_DEFINED);
            cmd.applyTo(currentProgram, monitor);
            return cmd.getFunction();
        } catch (Exception e) {
            println("CREATE_EXCEPTION " + addr + " " + e);
            return null;
        }
    }

    private int countRefs(Address a, boolean callsOnly, boolean[] sawDispatchTable) {
        int n = 0;
        ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(a);
        while (ri.hasNext()) {
            Reference r = ri.next();
            if (callsOnly && !r.getReferenceType().isCall()) {
                continue;
            }
            n++;
            Address from = r.getFromAddress();
            if (from != null) {
                long f = from.getOffset();
                if (f >= DISPATCH_TABLE && f < DISPATCH_TABLE + DISPATCH_SLOTS * 4) {
                    sawDispatchTable[0] = true;
                }
            }
        }
        return n;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 4) {
            printerr("usage: DefineActionHandlers.java <action_table.json> <audit.tsv> "
                    + "<new_functions.jsonl> <audit|apply> [limit]");
            return;
        }
        String tablePath = args[0];
        String auditPath = args[1];
        String jsonlPath = args[2];
        boolean apply = args[3].equals("apply");
        int limit = args.length > 4 ? Integer.parseInt(args[4]) : Integer.MAX_VALUE;

        JsonObject root = JsonParser.parseReader(new FileReader(tablePath)).getAsJsonObject();
        JsonArray actions = root.getAsJsonArray("actions");
        TreeMap<Long, Target> targets = new TreeMap<>();
        for (JsonElement e : actions) {
            JsonObject o = e.getAsJsonObject();
            Act a = new Act();
            a.id = o.get("id").getAsInt();
            a.name = o.get("name").getAsString();
            a.args = o.has("args") && !o.get("args").isJsonNull() ? o.get("args").getAsString() : "";
            a.handler = o.get("handler").getAsString();
            a.registeredBy = o.has("registered_by") ? o.get("registered_by").getAsString() : "";
            int us = a.handler.indexOf('_');
            long addr = Long.parseLong(a.handler.substring(us + 1), 16);
            Target t = targets.get(addr);
            if (t == null) {
                t = new Target();
                t.addr = addr;
                t.label = a.handler;
                targets.put(addr, t);
            }
            t.acts.add(a);
        }
        for (Target t : targets.values()) {
            Collections.sort(t.acts, Comparator.comparingInt(x -> x.id));
        }
        String dispatchHex = root.getAsJsonObject("dispatch").get("entry").getAsString();
        long dispatchAddr = Long.parseLong(dispatchHex, 16);

        println("MODE=" + (apply ? "apply" : "audit") + " PROGRAM=" + currentProgram.getName()
                + " ACTIONS=" + actions.size() + " UNIQUE_TARGETS=" + targets.size());
        if (actions.size() != 772) {
            printerr("unexpected action count: " + actions.size());
        }
        if (targets.size() != 565) {
            printerr("unexpected unique target count: " + targets.size());
        }

        FunctionManager fm = currentProgram.getFunctionManager();
        List<Row> rows = new ArrayList<>();
        Map<Long, Function> created = new LinkedHashMap<>();

        // Dispatcher is handled separately from handler coverage counts.
        Address da = toAddr(dispatchAddr);
        Row drow = new Row();
        drow.kind = "DISPATCHER";
        drow.addr = dispatchAddr;
        drow.label = "DISPATCH";
        Function df = fm.getFunctionAt(da);
        if (df != null) {
            drow.functionEntry = hex(df.getEntryPoint());
            drow.functionName = df.getName();
            if (!df.getName().equals(DISPATCHER_NAME)) {
                df.setName(DISPATCHER_NAME, SourceType.USER_DEFINED);
                drow.detail = "renamed from " + drow.functionName;
                drow.functionName = DISPATCHER_NAME;
            }
            drow.disposition = "already_function";
        } else {
            Function dcontain = fm.getFunctionContaining(da);
            if (dcontain != null) {
                drow.disposition = "interior";
                drow.detail = "inside " + dcontain.getName() + "@" + dcontain.getEntryPoint();
            } else {
                Instruction ins = currentProgram.getListing().getInstructionAt(da);
                if (ins == null) {
                    disassemble(da);
                    ins = currentProgram.getListing().getInstructionAt(da);
                }
                if (ins == null) {
                    drow.disposition = "undecodable";
                    drow.detail = "no instruction at dispatcher address";
                } else {
                    df = createAt(da, DISPATCHER_NAME, null);
                    if (df == null) {
                        drow.disposition = "create_failed";
                    } else {
                        drow.disposition = "created";
                        drow.functionEntry = hex(df.getEntryPoint());
                        drow.functionName = df.getName();
                        created.put(dispatchAddr, df);
                    }
                }
            }
        }
        if (df != null) {
            df.setName(DISPATCHER_NAME, SourceType.USER_DEFINED);
            String dc = "NWScript action dispatcher (vtable dispatch target).\n"
                    + "Action table vtable: 0x007450e4; slots: 772; handler functions named ACT_<id>_<name>."
                    + "\nHandler count/coverage: artifacts/analysis/ghidra-handlers/coverage.json";
            currentProgram.getListing().setComment(da, CommentType.PLATE, dc);
            drow.instructions = countInstructions(df.getBody());
            drow.size = df.getBody().getNumAddresses();
        }
        rows.add(drow);

        int audited = 0;
        for (Target t : targets.values()) {
            if (audited >= limit || monitor.isCancelled()) {
                break;
            }
            audited++;
            Address addr = toAddr(t.addr);
            Row r = new Row();
            r.kind = t.label.startsWith("FUN_") ? "FUN_TARGET" : "LAB_TARGET";
            r.addr = t.addr;
            r.label = t.label;
            r.primaryId = t.acts.get(0).id;
            r.primaryName = t.acts.get(0).name;
            StringBuilder ids = new StringBuilder();
            StringBuilder names = new StringBuilder();
            for (Act a : t.acts) {
                if (ids.length() > 0) {
                    ids.append(',');
                    names.append(';');
                }
                ids.append(a.id);
                names.append(a.name);
            }
            r.actionIds = ids.toString();
            r.actionNames = names.toString();

            Function at = fm.getFunctionAt(addr);
            if (at != null) {
                r.functionEntry = hex(at.getEntryPoint());
                r.functionName = at.getName();
                r.disposition = t.label.startsWith("FUN_") ? "already_defined_FUN" : "already_function";
                r.instructions = countInstructions(at.getBody());
                r.size = at.getBody().getNumAddresses();
                r.detail = "pre-existing function entry";
                rows.add(r);
                continue;
            }
            Function containing = fm.getFunctionContaining(addr);
            if (containing != null) {
                boolean createdThisRun = created.containsKey(containing.getEntryPoint().getOffset());
                r.disposition = createdThisRun ? "alias" : "interior";
                r.functionEntry = hex(containing.getEntryPoint());
                r.functionName = containing.getName();
                r.detail = createdThisRun
                        ? "inside handler created in this run (shared body / alias)"
                        : "inside existing function";
                rows.add(r);
                continue;
            }

            Instruction ins = currentProgram.getListing().getInstructionAt(addr);
            boolean didDisassemble = false;
            if (ins == null) {
                Instruction mid = currentProgram.getListing().getInstructionContaining(addr);
                if (mid != null) {
                    r.disposition = "unaligned_interior";
                    r.detail = "address inside instruction at " + hex(mid.getAddress());
                    rows.add(r);
                    continue;
                }
                disassemble(addr);
                didDisassemble = true;
                ins = currentProgram.getListing().getInstructionAt(addr);
            }
            if (ins == null) {
                r.disposition = "undecodable";
                r.detail = "disassembly produced no instruction";
                rows.add(r);
                continue;
            }

            boolean[] sawDispatch = new boolean[1];
            int anyRefs = countRefs(addr, false, sawDispatch);
            int callRefs = countRefs(addr, true, sawDispatch);
            byte[] pro = new byte[8];
            int proLen = 0;
            try {
                proLen = currentProgram.getMemory().getBytes(addr, pro);
            } catch (MemoryAccessException e) {
                proLen = 0;
            }
            String prologue = prologueKind(pro, proLen);

            AddressSetView body = CreateFunctionCmd.getFunctionBody(currentProgram, addr, monitor);
            if (body == null || body.getNumAddresses() == 0) {
                r.disposition = "rejected_flow";
                r.detail = "no computable function body; refs=" + anyRefs + " callRefs=" + callRefs;
                rows.add(r);
                continue;
            }
            int ic = 0;
            boolean bad = false;
            Instruction last = null;
            InstructionIterator ii = currentProgram.getListing().getInstructions(body, true);
            while (ii.hasNext()) {
                Instruction x = ii.next();
                last = x;
                ic++;
                if (x.getLength() <= 0) {
                    bad = true;
                }
                if (ic > 20000) {
                    bad = true;
                    break;
                }
            }
            if (ic == 0 || last == null || bad || !isTerminal(last)) {
                r.disposition = "rejected_flow";
                r.detail = "instructions=" + ic + " terminal=" + (last != null && isTerminal(last))
                        + " refs=" + anyRefs + " callRefs=" + callRefs + " prologue=" + prologue;
                rows.add(r);
                continue;
            }

            String name = "ACT_" + t.acts.get(0).id + "_" + sanitize(t.acts.get(0).name);
            SymbolIterator existing = currentProgram.getSymbolTable().getSymbols(name);
            if (existing.hasNext()) {
                name = name + "_" + Long.toHexString(t.addr);
            }
            Function nf = createAt(addr, name, body);
            if (nf == null) {
                r.disposition = "create_failed";
                r.detail = "CreateFunctionCmd returned no function; instructions=" + ic
                        + " refs=" + anyRefs + " callRefs=" + callRefs;
                rows.add(r);
                continue;
            }
            int nic = countInstructions(nf.getBody());
            Instruction nlast = null;
            InstructionIterator ni = currentProgram.getListing().getInstructions(nf.getBody(), true);
            while (ni.hasNext()) {
                nlast = ni.next();
            }
            if (nic == 0 || nlast == null || !isTerminal(nlast)) {
                fm.removeFunction(addr);
                r.disposition = "rejected_after_create";
                r.detail = "created body instructions=" + nic;
                rows.add(r);
                continue;
            }
            created.put(t.addr, nf);
            r.disposition = "created";
            r.functionEntry = hex(nf.getEntryPoint());
            r.functionName = nf.getName();
            r.instructions = nic;
            r.size = nf.getBody().getNumAddresses();
            r.detail = "justification=" + (prologue.isEmpty() ? "dispatch_ref_valid_flow" : prologue)
                    + "; refs=" + anyRefs + "; callRefs=" + callRefs
                    + "; tableRef=" + sawDispatch[0] + "; disassembled=" + didDisassemble;

            StringBuilder c = new StringBuilder();
            c.append("NWScript action handler (dispatch table 0x007450e4, ").append(DISPATCH_SLOTS)
                    .append(" slots).\n");
            c.append("Primary action: ").append(t.acts.get(0).id).append(" ")
                    .append(t.acts.get(0).name);
            if (!t.acts.get(0).args.isEmpty()) {
                c.append("(").append(t.acts.get(0).args).append(")");
            }
            c.append("\nShared by ").append(t.acts.size()).append(" action(s): ");
            for (int i = 0; i < t.acts.size(); i++) {
                if (i > 0) {
                    c.append("; ");
                }
                Act a = t.acts.get(i);
                c.append(a.id).append(" ").append(a.name);
            }
            c.append("\nHandler label: ").append(t.label);
            currentProgram.getListing().setComment(addr, CommentType.PLATE, c.toString());
            rows.add(r);
        }

        // Any target skipped because of limit stays unrecorded; report it.
        int unprocessed = targets.size() - audited;
        println("AUDITED=" + audited + " UNPROCESSED=" + unprocessed);

        int decompiled = 0;
        int failed = 0;
        List<String> jsonLines = new ArrayList<>();
        if (apply && !created.isEmpty()) {
            DecompInterface di = new DecompInterface();
            di.toggleCCode(true);
            di.toggleSyntaxTree(false);
            di.setSimplificationStyle("decompile");
            if (!di.openProgram(currentProgram)) {
                printerr("decompiler failed to open program: " + di.getLastMessage());
            } else {
                List<Map.Entry<Long, Function>> entries = new ArrayList<>(created.entrySet());
                entries.sort(Map.Entry.comparingByKey());
                for (Map.Entry<Long, Function> e : entries) {
                    if (monitor.isCancelled()) {
                        break;
                    }
                    Function f = e.getValue();
                    Target t = targets.get(e.getKey());
                    Row r = findRow(rows, e.getKey());
                    StringBuilder b = new StringBuilder(1024);
                    b.append("{\"entry\":\"").append(hex(f.getEntryPoint())).append("\"");
                    b.append(",\"name\":\"").append(esc(f.getName())).append("\"");
                    b.append(",\"kind\":\"").append(e.getKey() == dispatchAddr ? "dispatcher" : "handler")
                            .append("\"");
                    b.append(",\"size\":").append(f.getBody().getNumAddresses());
                    b.append(",\"instructions\":").append(countInstructions(f.getBody()));
                    if (t != null) {
                        b.append(",\"handler_label\":\"").append(esc(t.label)).append("\"");
                        b.append(",\"primary_action_id\":").append(t.acts.get(0).id);
                        b.append(",\"primary_action_name\":\"").append(esc(t.acts.get(0).name)).append("\"");
                        b.append(",\"action_count\":").append(t.acts.size());
                    }
                    String error = null;
                    String code = null;
                    try {
                        DecompileResults res = di.decompileFunction(f, 60, monitor);
                        if (res != null && res.decompileCompleted() && res.getDecompiledFunction() != null) {
                            code = res.getDecompiledFunction().getC();
                            if (code == null || code.trim().isEmpty()) {
                                error = "empty pseudocode";
                                code = null;
                            }
                        } else {
                            error = res == null ? "null decompile result" : res.getErrorMessage();
                        }
                    } catch (Exception ex) {
                        error = String.valueOf(ex);
                    }
                    if (code != null) {
                        b.append(",\"pseudocode_sha256\":\"").append(sha256(code)).append("\"");
                        b.append(",\"pseudocode\":\"").append(esc(code)).append("\"");
                        decompiled++;
                        if (r != null) {
                            r.detail = r.detail + "; decompiled=yes";
                        }
                    } else {
                        b.append(",\"pseudocode_error\":\"").append(esc(error)).append("\"");
                        failed++;
                        if (r != null) {
                            r.detail = r.detail + "; decompiled=no:" + error;
                        }
                    }
                    b.append("}");
                    jsonLines.add(b.toString());
                }
                di.dispose();
            }
        }

        try (PrintWriter out = new PrintWriter(auditPath, "UTF-8")) {
            out.println("kind\taddress\thandler_label\tfunction_entry\tfunction_name\tprimary_action_id"
                    + "\tprimary_action_name\taction_ids\taction_names\tdisposition\tinstructions\tsize\tdetail");
            for (Row r : rows) {
                out.println(tsv(r.kind) + "\t" + String.format("0x%08x", r.addr) + "\t" + tsv(r.label)
                        + "\t" + tsv(r.functionEntry) + "\t" + tsv(r.functionName) + "\t" + r.primaryId
                        + "\t" + tsv(r.primaryName) + "\t" + tsv(r.actionIds) + "\t" + tsv(r.actionNames)
                        + "\t" + tsv(r.disposition) + "\t" + r.instructions + "\t" + r.size + "\t"
                        + tsv(r.detail));
            }
        }
        try (PrintWriter out = new PrintWriter(jsonlPath, "UTF-8")) {
            for (String line : jsonLines) {
                out.println(line);
            }
        }

        Map<String, Integer> counts = new TreeMap<>();
        for (Row r : rows) {
            counts.merge(r.disposition, 1, Integer::sum);
        }
        println("SUMMARY rows=" + rows.size() + " created=" + created.size() + " decompiled=" + decompiled
                + " failed=" + failed + " unprocessed=" + unprocessed);
        println("DISPOSITIONS=" + counts);
        println("AUDIT_TSV=" + auditPath);
        println("NEW_FUNCTIONS_JSONL=" + jsonlPath);
        if (!apply) {
            println("AUDIT_ONLY: no decompilation; use -readOnly to avoid persistence");
        }
    }

    private int countInstructions(AddressSetView body) {
        int n = 0;
        InstructionIterator it = currentProgram.getListing().getInstructions(body, true);
        while (it.hasNext()) {
            it.next();
            n++;
        }
        return n;
    }

    private Row findRow(List<Row> rows, long addr) {
        for (Row r : rows) {
            if (r.addr == addr) {
                return r;
            }
        }
        return null;
    }
}
