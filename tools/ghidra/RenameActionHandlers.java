// Sprint 0 lane 07: rename the 125 pre-existing FUN_ NWScript action-handler targets
// to ACT_<primary_id>_<sanitized_name>, attach a plate comment carrying every shared
// action id/name, re-decompile the renamed targets and export metadata.
//
// Runs only against a PRIVATE COPY of the enhanced handler project.
// Usage (analyzeHeadless, no analysis, JDK 21):
//   analyzeHeadless <projectDir> kotor -process swkotor.exe -noanalysis \
//     -scriptPath artifacts/lanes/07-ghidra-rename/scripts \
//     -postScript RenameActionHandlers.java <action_table.json> <renamed_functions.jsonl> \
//         <apply_summary.json>
// @category KotOR

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

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
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class RenameActionHandlers extends GhidraScript {

    private static final String DISPATCH_TABLE = "0x007450e4";
    private static final int DISPATCH_SLOTS = 772;
    private static final String DISPATCHER_NAME = "NWScript_DispatchAction";

    private static class Act {
        int id;
        String name;
        String args;
        String handler;
    }

    private static class Target {
        long addr;
        String label;
        List<Act> acts = new ArrayList<>();
        String proposedName;
        int nameResolved; // 0 = as proposed, 1 = suffixed for uniqueness
    }

    private static class Row {
        long addr;
        String oldName = "";
        String newName = "";
        boolean renamed;
        String disposition = "";
        String note = "";
        int actionCount;
        int primaryId;
        String primaryName = "";
        long size = -1;
        int instructions = -1;
        String pseudocodeSha = "";
        String pseudocodeError = "";
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
        byte[] digest = md.digest(text.getBytes(StandardCharsets.UTF_8));
        StringBuilder b = new StringBuilder();
        for (byte x : digest) {
            b.append(String.format("%02x", x));
        }
        return b.toString();
    }

    private static String hex(long v) {
        return String.format("0x%08x", v);
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

    private int countInstructions(AddressSetView body) {
        int n = 0;
        InstructionIterator it = currentProgram.getListing().getInstructions(body, true);
        while (it.hasNext()) {
            it.next();
            n++;
        }
        return n;
    }

    private String buildComment(Target t, String oldName) {
        StringBuilder c = new StringBuilder();
        c.append("NWScript action handler (dispatch table ").append(DISPATCH_TABLE)
                .append(", ").append(DISPATCH_SLOTS).append(" slots).\n");
        Act p = t.acts.get(0);
        c.append("Primary action: ").append(p.id).append(" ").append(p.name);
        if (p.args != null && !p.args.isEmpty()) {
            c.append("(").append(p.args).append(")");
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
        c.append("\nRenamed from ").append(oldName)
                .append(" by Sprint 0 lane 07 (ACT_<primary_id>_<name> convention).");
        return c.toString();
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 3) {
            printerr("usage: RenameActionHandlers.java <action_table.json> <renamed_functions.jsonl> "
                    + "<apply_summary.json>");
            return;
        }
        String tablePath = args[0];
        String jsonlPath = args[1];
        String summaryPath = args[2];

        JsonObject root;
        try (BufferedReader r = new BufferedReader(new FileReader(tablePath, StandardCharsets.UTF_8))) {
            root = JsonParser.parseReader(r).getAsJsonObject();
        }
        JsonArray actions = root.getAsJsonArray("actions");
        TreeMap<Long, Target> targets = new TreeMap<>();
        for (JsonElement e : actions) {
            JsonObject o = e.getAsJsonObject();
            Act a = new Act();
            a.id = o.get("id").getAsInt();
            a.name = o.get("name").getAsString();
            a.args = o.has("args") && !o.get("args").isJsonNull() ? o.get("args").getAsString() : "";
            a.handler = o.get("handler").getAsString();
            int us = a.handler.indexOf('_');
            long addr = Long.parseLong(a.handler.substring(us + 1), 16);
            Target t = targets.get(addr);
            if (t == null) {
                t = new Target();
                t.addr = addr;
                t.label = a.handler;
                targets.put(addr, t);
            } else if (!t.label.equals(a.handler)) {
                printerr("MIXED_HANDLER_LABELS at " + hex(addr) + ": " + t.label + " vs " + a.handler);
            }
            t.acts.add(a);
        }
        for (Target t : targets.values()) {
            Collections.sort(t.acts, Comparator.comparingInt(x -> x.id));
        }

        FunctionManager fm = currentProgram.getFunctionManager();
        int functionCountBefore = fm.getFunctionCount();
        int externalBefore = 0;
        for (Function f : fm.getExternalFunctions()) {
            externalBefore++;
        }
        println("PROGRAM=" + currentProgram.getName() + " SHA256=" + currentProgram.getExecutableSHA256());
        println("ACTIONS=" + actions.size() + " UNIQUE_TARGETS=" + targets.size()
                + " FUNCTIONS=" + functionCountBefore + " EXTERNAL=" + externalBefore);
        if (targets.size() != 565) {
            printerr("unexpected unique target count: " + targets.size());
        }

        // Build proposed names for the pre-existing FUN_ handler targets only.
        Map<String, Long> nameOwners = new LinkedHashMap<>();
        for (Target t : targets.values()) {
            if (!t.label.startsWith("FUN_")) {
                continue;
            }
            t.proposedName = "ACT_" + t.acts.get(0).id + "_" + sanitize(t.acts.get(0).name);
            Long prior = nameOwners.get(t.proposedName);
            if (prior != null) {
                printerr("DUPLICATE_PROPOSED_NAME " + t.proposedName + " " + hex(prior) + " / " + hex(t.addr));
                t.proposedName = t.proposedName + "_" + Long.toHexString(t.addr);
                t.nameResolved = 1;
            }
            SymbolIterator existing = currentProgram.getSymbolTable().getSymbols(t.proposedName);
            while (existing.hasNext()) {
                Symbol s = existing.next();
                long sa = s.getAddress().getOffset();
                if (sa != t.addr) {
                    printerr("NAME_COLLIDES_WITH_EXISTING " + t.proposedName + " at " + hex(sa)
                            + " (target " + hex(t.addr) + ")");
                    t.proposedName = t.proposedName + "_" + Long.toHexString(t.addr);
                    t.nameResolved = 1;
                    break;
                }
            }
            nameOwners.put(t.proposedName, t.addr);
        }

        // Rename and comment only the pre-existing FUN_ targets.
        List<Row> rows = new ArrayList<>();
        int renamed = 0;
        int already = 0;
        int renameFailures = 0;
        int commentFailures = 0;
        int targetMissing = 0;
        int nameResolvedCount = 0;
        Set<String> targetNames = new LinkedHashSet<>();
        for (Target t : targets.values()) {
            Address addr = toAddr(t.addr);
            Function f = fm.getFunctionAt(addr);
            if (f == null) {
                targetMissing++;
                printerr("TARGET_HAS_NO_FUNCTION_AT_ENTRY " + t.label + " " + hex(t.addr));
                continue;
            }
            if (!t.label.startsWith("FUN_")) {
                targetNames.add(f.getName());
                continue;
            }
            Row r = new Row();
            r.addr = t.addr;
            r.oldName = f.getName();
            r.actionCount = t.acts.size();
            r.primaryId = t.acts.get(0).id;
            r.primaryName = t.acts.get(0).name;
            if (!r.oldName.startsWith("FUN_")) {
                r.disposition = "skipped_not_fun";
                r.note = "function at handler target is named " + r.oldName;
                r.newName = r.oldName;
                rows.add(r);
                targetNames.add(r.newName);
                continue;
            }
            try {
                if (r.oldName.equals(t.proposedName)) {
                    r.disposition = "already_named";
                    already++;
                } else {
                    f.setName(t.proposedName, SourceType.USER_DEFINED);
                    r.disposition = "renamed";
                    renamed++;
                }
            } catch (Exception ex) {
                r.disposition = "rename_failed";
                r.note = String.valueOf(ex);
                renameFailures++;
                r.newName = f.getName();
                rows.add(r);
                targetNames.add(r.newName);
                printerr("RENAME_FAILED " + r.oldName + " -> " + t.proposedName + ": " + ex);
                continue;
            }
            String now = f.getName();
            r.newName = now;
            if (!now.equals(t.proposedName)) {
                r.disposition = "rename_mismatch";
                r.note = "expected " + t.proposedName + ", got " + now;
                renameFailures++;
                printerr("RENAME_MISMATCH " + hex(t.addr) + " expected " + t.proposedName + " got " + now);
            }
            r.size = f.getBody().getNumAddresses();
            r.instructions = countInstructions(f.getBody());
            String comment = buildComment(t, r.oldName);
            try {
                currentProgram.getListing().setComment(addr, CommentType.PLATE, comment);
                String back = currentProgram.getListing().getComment(CommentType.PLATE, addr);
                boolean all = back != null;
                if (all) {
                    for (Act a : t.acts) {
                        if (!back.contains(a.id + " " + a.name)) {
                            all = false;
                            break;
                        }
                    }
                }
                if (!all) {
                    commentFailures++;
                    r.note = (r.note.isEmpty() ? "" : r.note + "; ") + "comment-readback-missing-action";
                    printerr("COMMENT_READBACK_FAILED " + hex(t.addr));
                }
            } catch (Exception ex) {
                commentFailures++;
                r.note = (r.note.isEmpty() ? "" : r.note + "; ") + "comment-exception:" + ex;
                printerr("COMMENT_FAILED " + hex(t.addr) + ": " + ex);
            }
            if (t.nameResolved != 0) {
                nameResolvedCount++;
            }
            rows.add(r);
            targetNames.add(r.newName);
        }

        // Dispatcher check (must remain present and correctly named).
        long dispatchAddr = Long.parseLong(
                root.getAsJsonObject("dispatch").get("entry").getAsString(), 16);
        Function df = fm.getFunctionAt(toAddr(dispatchAddr));
        boolean dispatcherPresent = df != null;
        boolean dispatcherNamed = df != null && DISPATCHER_NAME.equals(df.getName());
        String dispatcherComment = df == null ? null
                : currentProgram.getListing().getComment(CommentType.PLATE, toAddr(dispatchAddr));
        if (!dispatcherPresent || !dispatcherNamed) {
            printerr("DISPATCHER_CHECK_FAILED present=" + dispatcherPresent + " named=" + dispatcherNamed);
        }

        // Re-decompile every renamed target (and detect decompiler open failure once).
        int decompiled = 0;
        int decompileFailed = 0;
        DecompInterface di = new DecompInterface();
        di.toggleCCode(true);
        di.toggleSyntaxTree(false);
        di.setSimplificationStyle("decompile");
        if (!di.openProgram(currentProgram)) {
            printerr("DECOMPILER_OPEN_FAILED " + di.getLastMessage());
            for (Row r : rows) {
                if (r.disposition.equals("renamed") || r.disposition.equals("already_named")
                        || r.disposition.equals("rename_mismatch")) {
                    r.pseudocodeError = "decompiler open failed: " + di.getLastMessage();
                    decompileFailed++;
                }
            }
        } else {
            for (Row r : rows) {
                if (!(r.disposition.equals("renamed") || r.disposition.equals("already_named")
                        || r.disposition.equals("rename_mismatch"))) {
                    continue;
                }
                Function f = fm.getFunctionAt(toAddr(r.addr));
                String code = null;
                String error = null;
                try {
                    DecompileResults res = di.decompileFunction(f, 120, monitor);
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
                    r.pseudocodeSha = sha256(code);
                    decompiled++;
                } else {
                    r.pseudocodeError = error == null ? "unknown" : error;
                    decompileFailed++;
                    printerr("DECOMPILE_FAILED " + r.newName + " " + r.pseudocodeError);
                }
            }
            di.dispose();
        }

        // Export one JSONL row per renamed handler target.
        try (PrintWriter out = new PrintWriter(jsonlPath, "UTF-8")) {
            for (Row r : rows) {
                if (!(r.disposition.equals("renamed") || r.disposition.equals("already_named")
                        || r.disposition.equals("rename_mismatch"))) {
                    continue;
                }
                Target t = targets.get(r.addr);
                StringBuilder b = new StringBuilder(1024);
                b.append("{\"entry\":\"").append(hex(r.addr)).append("\"");
                b.append(",\"old_name\":\"").append(esc(r.oldName)).append("\"");
                b.append(",\"new_name\":\"").append(esc(r.newName)).append("\"");
                b.append(",\"primary_action_id\":").append(r.primaryId);
                b.append(",\"primary_action_name\":\"").append(esc(r.primaryName)).append("\"");
                b.append(",\"action_count\":").append(r.actionCount);
                b.append(",\"actions\":[");
                for (int i = 0; i < t.acts.size(); i++) {
                    Act a = t.acts.get(i);
                    if (i > 0) {
                        b.append(',');
                    }
                    b.append("{\"id\":").append(a.id)
                            .append(",\"name\":\"").append(esc(a.name)).append("\"")
                            .append(",\"args\":\"").append(esc(a.args)).append("\"}");
                }
                b.append("]");
                b.append(",\"size\":").append(r.size);
                b.append(",\"instructions\":").append(r.instructions);
                if (!r.pseudocodeSha.isEmpty()) {
                    b.append(",\"pseudocode_sha256\":\"").append(r.pseudocodeSha).append("\"");
                } else {
                    b.append(",\"pseudocode_error\":\"").append(esc(r.pseudocodeError)).append("\"");
                }
                b.append(",\"disposition\":\"").append(r.disposition).append("\"");
                b.append("}");
                out.println(b.toString());
            }
        }

        int functionCountAfter = fm.getFunctionCount();
        int externalAfter = 0;
        for (Function f : fm.getExternalFunctions()) {
            externalAfter++;
        }
        int duplicateTargetNames = targetNames.size() < (targets.size() - targetMissing)
                ? (targets.size() - targetMissing) - targetNames.size() : 0;
        boolean allNamed = true;
        for (Target t : targets.values()) {
            Function f = fm.getFunctionAt(toAddr(t.addr));
            if (f == null) {
                allNamed = false;
                continue;
            }
            String expected = t.proposedName != null ? t.proposedName
                    : "ACT_" + t.acts.get(0).id + "_" + sanitize(t.acts.get(0).name);
            if (!f.getName().equals(expected)) {
                allNamed = false;
                printerr("TARGET_NAME_NOT_EXPECTED " + hex(t.addr) + " expected " + expected
                        + " got " + f.getName());
            }
        }

        JsonObject summary = new JsonObject();
        summary.addProperty("program", currentProgram.getName());
        summary.addProperty("program_sha256", currentProgram.getExecutableSHA256());
        summary.addProperty("actions", actions.size());
        summary.addProperty("unique_targets", targets.size());
        summary.addProperty("fun_targets", nameOwners.size());
        summary.addProperty("lab_targets", targets.size() - nameOwners.size());
        summary.addProperty("renamed", renamed);
        summary.addProperty("already_named", already);
        summary.addProperty("rename_failures", renameFailures);
        summary.addProperty("comment_failures", commentFailures);
        summary.addProperty("targets_missing_function", targetMissing);
        summary.addProperty("name_collisions_resolved", nameResolvedCount);
        summary.addProperty("target_name_set_size", targetNames.size());
        summary.addProperty("duplicate_target_names", duplicateTargetNames);
        summary.addProperty("all_targets_named_expected", allNamed);
        summary.addProperty("function_count_before", functionCountBefore);
        summary.addProperty("function_count_after", functionCountAfter);
        summary.addProperty("external_before", externalBefore);
        summary.addProperty("external_after", externalAfter);
        summary.addProperty("dispatcher_present", dispatcherPresent);
        summary.addProperty("dispatcher_named", dispatcherNamed);
        summary.addProperty("dispatcher_plate_comment_present", dispatcherComment != null);
        summary.addProperty("decompiled", decompiled);
        summary.addProperty("decompile_failed", decompileFailed);
        summary.addProperty("jsonl_rows", rows.size());
        try (PrintWriter out = new PrintWriter(summaryPath, "UTF-8")) {
            out.println(new GsonBuilder().setPrettyPrinting().create().toJson(summary));
        }

        println("RENAMED=" + renamed + " ALREADY=" + already + " FAILURES=" + renameFailures
                + " COMMENTS_FAILED=" + commentFailures + " MISSING_TARGETS=" + targetMissing
                + " NAME_COLLISIONS_RESOLVED=" + nameResolvedCount);
        println("TARGET_NAMES=" + targetNames.size() + " DUPLICATES=" + duplicateTargetNames
                + " ALL_NAMED_EXPECTED=" + allNamed);
        println("FUNCTIONS_BEFORE=" + functionCountBefore + " AFTER=" + functionCountAfter
                + " EXTERNAL=" + externalAfter);
        println("DISPATCHER present=" + dispatcherPresent + " named=" + dispatcherNamed);
        println("DECOMPILED=" + decompiled + " FAILED=" + decompileFailed);
        println("JSONL=" + jsonlPath);
        println("APPLY_SUMMARY=" + summaryPath);
    }
}
