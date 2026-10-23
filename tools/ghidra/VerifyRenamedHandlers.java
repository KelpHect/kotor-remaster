// Sprint 0 lane 07 independent verification of the FUN_ handler rename pass.
// Opens the saved lane project read-only, validates every target/action mapping,
// renamed comments, decompiled metadata hashes, dispatcher, and baseline retention.
// Writes coverage.json and verification.json.
//
// Usage:
//   analyzeHeadless <projectDir> kotor -process swkotor.exe -noanalysis -readOnly \
//     -scriptPath artifacts/lanes/07-ghidra-rename/scripts \
//     -postScript VerifyRenamedHandlers.java <action_table.json> <baseline_functions.jsonl> \
//         <renamed_functions.jsonl> <coverage.json> <verification.json> [apply_summary.json]
// @category KotOR

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileReader;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
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

public class VerifyRenamedHandlers extends GhidraScript {

    private static final String DISPATCHER_NAME = "NWScript_DispatchAction";
    private static final String DISPATCH_TABLE = "0x007450e4";

    private static class Act {
        int id;
        String name;
        String args;
        String handler;
    }

    private static class Target {
        long addr;
        String label;
        String kind;
        List<Act> acts = new ArrayList<>();
    }

    private static String hex(long v) {
        return String.format("0x%08x", v);
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

    private static String sha256File(String path) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            try (FileInputStream in = new FileInputStream(new File(path))) {
                byte[] buf = new byte[1 << 16];
                int n;
                while ((n = in.read(buf)) > 0) {
                    md.update(buf, 0, n);
                }
            }
            StringBuilder b = new StringBuilder();
            for (byte x : md.digest()) {
                b.append(String.format("%02x", x));
            }
            return b.toString();
        } catch (Exception e) {
            return "";
        }
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

    private static String expectedName(Target t) {
        return "ACT_" + t.acts.get(0).id + "_" + sanitize(t.acts.get(0).name);
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

    private void check(JsonArray checks, String name, boolean pass, String detail) {
        JsonObject o = new JsonObject();
        o.addProperty("check", name);
        o.addProperty("pass", pass);
        o.addProperty("detail", detail);
        checks.add(o);
        println((pass ? "CHECK_PASS " : "CHECK_FAIL ") + name + " :: " + detail);
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 5) {
            printerr("usage: VerifyRenamedHandlers.java <action_table.json> <baseline_functions.jsonl> "
                    + "<renamed_functions.jsonl> <coverage.json> <verification.json> [apply_summary.json]");
            return;
        }
        String tablePath = args[0];
        String baselinePath = args[1];
        String jsonlPath = args[2];
        String coveragePath = args[3];
        String verificationPath = args[4];
        String applySummaryPath = args.length > 5 ? args[5] : null;

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
            long addr = Long.parseLong(a.handler.substring(a.handler.indexOf('_') + 1), 16);
            Target t = targets.get(addr);
            if (t == null) {
                t = new Target();
                t.addr = addr;
                t.label = a.handler;
                t.kind = t.label.startsWith("FUN_") ? "FUN" : "LAB";
                targets.put(addr, t);
            }
            t.acts.add(a);
        }
        for (Target t : targets.values()) {
            Collections.sort(t.acts, Comparator.comparingInt(x -> x.id));
        }
        Set<Long> handlerAddrs = new LinkedHashSet<>(targets.keySet());

        FunctionManager fm = currentProgram.getFunctionManager();
        int totalFunctions = fm.getFunctionCount();
        int externalFunctions = 0;
        for (Function f : fm.getExternalFunctions()) {
            externalFunctions++;
        }
        int internalFunctions = 0;
        for (Function f : fm.getFunctions(true)) {
            if (!f.isExternal()) {
                internalFunctions++;
            }
        }

        JsonArray checks = new JsonArray();
        JsonArray gaps = new JsonArray();

        // Handler targets: exact function, expected ACT_ name, plate comment with every action.
        int targetsWithFunction = 0;
        int targetsNameExpected = 0;
        int targetsCommentOk = 0;
        int funTargets = 0;
        int labTargets = 0;
        int actionsMapped = 0;
        Set<String> targetNames = new LinkedHashSet<>();
        JsonArray coverageTargets = new JsonArray();
        List<String> missingTargets = new ArrayList<>();
        List<String> nameProblems = new ArrayList<>();
        List<String> commentProblems = new ArrayList<>();
        for (Target t : targets.values()) {
            Address addr = toAddr(t.addr);
            Function f = fm.getFunctionAt(addr);
            boolean present = f != null;
            boolean namedExpected = false;
            int size = -1;
            int instructions = -1;
            boolean commentOk = false;
            String actualName = "";
            if (present) {
                targetsWithFunction++;
                actualName = f.getName();
                namedExpected = actualName.equals(expectedName(t));
                if (namedExpected) {
                    targetsNameExpected++;
                } else {
                    nameProblems.add(hex(t.addr) + " expected " + expectedName(t) + " got " + actualName);
                }
                targetNames.add(actualName);
                size = (int) f.getBody().getNumAddresses();
                instructions = countInstructions(f.getBody());
                String comment = currentProgram.getListing().getComment(CommentType.PLATE, addr);
                commentOk = comment != null;
                if (commentOk) {
                    for (Act a : t.acts) {
                        if (!comment.contains(a.id + " " + a.name)) {
                            commentOk = false;
                            break;
                        }
                    }
                }
                if (commentOk) {
                    targetsCommentOk++;
                } else {
                    commentProblems.add(t.label);
                }
            } else {
                missingTargets.add(t.label + " at " + hex(t.addr));
            }
            if (t.kind.equals("FUN")) {
                funTargets++;
            } else {
                labTargets++;
            }
            actionsMapped += t.acts.size();

            JsonObject row = new JsonObject();
            row.addProperty("entry", hex(t.addr));
            row.addProperty("handler_label", t.label);
            row.addProperty("target_kind", t.kind);
            row.addProperty("primary_action_id", t.acts.get(0).id);
            row.addProperty("primary_action_name", t.acts.get(0).name);
            row.addProperty("action_count", t.acts.size());
            JsonArray actArray = new JsonArray();
            for (Act a : t.acts) {
                JsonObject ao = new JsonObject();
                ao.addProperty("id", a.id);
                ao.addProperty("name", a.name);
                ao.addProperty("args", a.args);
                actArray.add(ao);
            }
            row.add("actions", actArray);
            row.addProperty("function_present", present);
            row.addProperty("function_name", actualName);
            row.addProperty("expected_name", expectedName(t));
            row.addProperty("named_expected", namedExpected);
            if (present) {
                row.addProperty("size", size);
                row.addProperty("instructions", instructions);
            }
            row.addProperty("plate_comment_ok", commentOk);
            row.addProperty("renamed_by_lane07", t.kind.equals("FUN") && namedExpected);
            coverageTargets.add(row);
        }

        check(checks, "unique_targets_565", targets.size() == 565,
                "targets=" + targets.size());
        check(checks, "all_565_targets_have_functions", targetsWithFunction == targets.size(),
                "with_function=" + targetsWithFunction + "/" + targets.size()
                        + (missingTargets.isEmpty() ? "" : " missing=" + missingTargets.size()));
        check(checks, "all_565_target_names_expected", targetsNameExpected == targets.size(),
                "named=" + targetsNameExpected + "/" + targets.size()
                        + (nameProblems.isEmpty() ? "" : " problems=" + nameProblems.size()));
        check(checks, "target_function_names_unique", targetNames.size() == targets.size(),
                "distinct_names=" + targetNames.size() + " targets=" + targets.size());
        check(checks, "all_565_target_comments_list_actions", targetsCommentOk == targets.size(),
                "comments_ok=" + targetsCommentOk + "/" + targets.size()
                        + (commentProblems.isEmpty() ? "" : " problems=" + commentProblems.size()));
        check(checks, "772_actions_map_to_existing_targets",
                actionsMapped == 772 && actions.size() == 772 && targetsWithFunction == 565,
                "actions=" + actions.size() + " mapped=" + actionsMapped);
        check(checks, "fun_targets_125_lab_targets_440", funTargets == 125 && labTargets == 440,
                "FUN=" + funTargets + " LAB=" + labTargets);

        // Dispatcher.
        long dispatchAddr = Long.parseLong(
                root.getAsJsonObject("dispatch").get("entry").getAsString(), 16);
        Function df = fm.getFunctionAt(toAddr(dispatchAddr));
        boolean dispatcherPresent = df != null;
        boolean dispatcherNamed = df != null && DISPATCHER_NAME.equals(df.getName());
        String dcomment = df == null ? null
                : currentProgram.getListing().getComment(CommentType.PLATE, toAddr(dispatchAddr));
        boolean dispatcherCommentOk = dcomment != null && dcomment.contains("772")
                && dcomment.contains(DISPATCH_TABLE);
        check(checks, "dispatcher_present_named_commented",
                dispatcherPresent && dispatcherNamed && dispatcherCommentOk,
                "present=" + dispatcherPresent + " named=" + dispatcherNamed
                        + " comment=" + (dcomment != null) + " entry=" + hex(dispatchAddr));

        // Baseline retention from the pre-enhancement export.
        int baselineRows = 0;
        int baselinePresent = 0;
        int baselineNameUnchanged = 0;
        int baselineHandlersSkipped = 0;
        List<String> baselineMissing = new ArrayList<>();
        List<String> baselineRenamed = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(baselinePath, StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                if (line.trim().isEmpty()) {
                    continue;
                }
                baselineRows++;
                JsonObject o = JsonParser.parseString(line).getAsJsonObject();
                String entry = o.get("entry").getAsString();
                String baseName = o.get("name").getAsString();
                boolean external = o.has("external") && o.get("external").getAsBoolean();
                if (external) {
                    continue;
                }
                long a = Long.parseLong(entry, 16);
                Function f = fm.getFunctionAt(toAddr(a));
                if (f == null) {
                    if (baselineMissing.size() < 50) {
                        baselineMissing.add(entry + " " + baseName);
                    }
                    continue;
                }
                baselinePresent++;
                if (handlerAddrs.contains(a)) {
                    baselineHandlersSkipped++;
                    continue;
                }
                if (f.getName().equals(baseName)) {
                    baselineNameUnchanged++;
                } else if (baselineRenamed.size() < 50) {
                    baselineRenamed.add(entry + " " + baseName + " -> " + f.getName());
                }
            }
        }
        int baselineNonHandlers = baselineRows - 125;
        check(checks, "no_baseline_function_lost", baselineRows == 8616 && baselinePresent == baselineRows,
                "baseline=" + baselineRows + " present=" + baselinePresent
                        + " missing=" + baselineMissing.size());
        check(checks, "no_non_handler_baseline_renamed",
                baselineNameUnchanged == baselineNonHandlers && baselineRenamed.isEmpty(),
                "unchanged=" + baselineNameUnchanged + "/" + baselineNonHandlers
                        + " handlers_skipped=" + baselineHandlersSkipped);
        check(checks, "function_counts_9407_9057_350",
                totalFunctions == 9407 && internalFunctions == 9057 && externalFunctions == 350,
                "total=" + totalFunctions + " internal=" + internalFunctions
                        + " external=" + externalFunctions);

        // Renamed-target export: identity, actions, sizes and re-decompiled pseudocode hashes.
        Map<Long, JsonObject> exported = new HashMap<>();
        int jsonlRows = 0;
        List<String> jsonlProblems = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(jsonlPath, StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                if (line.trim().isEmpty()) {
                    continue;
                }
                jsonlRows++;
                JsonObject o = JsonParser.parseString(line).getAsJsonObject();
                long a = Long.parseLong(o.get("entry").getAsString().substring(2), 16);
                if (exported.put(a, o) != null) {
                    jsonlProblems.add("duplicate entry " + hex(a));
                }
            }
        }
        Set<Long> funAddrs = new LinkedHashSet<>();
        for (Target t : targets.values()) {
            if (t.kind.equals("FUN")) {
                funAddrs.add(t.addr);
            }
        }
        boolean jsonlSetOk = exported.keySet().equals(funAddrs);
        if (!jsonlSetOk) {
            jsonlProblems.add("entry set mismatch exported=" + exported.size() + " expected=" + funAddrs.size());
        }
        int jsonlIdentityOk = 0;
        int jsonlActionsOk = 0;
        int jsonlMetadataOk = 0;
        int redecompiled = 0;
        int decompileFailed = 0;
        int pseudocodeMatch = 0;
        DecompInterface di = new DecompInterface();
        di.toggleCCode(true);
        di.toggleSyntaxTree(false);
        di.setSimplificationStyle("decompile");
        boolean open = di.openProgram(currentProgram);
        if (!open) {
            printerr("DECOMPILER_OPEN_FAILED " + di.getLastMessage());
        }
        for (Target t : targets.values()) {
            if (!t.kind.equals("FUN")) {
                continue;
            }
            JsonObject o = exported.get(t.addr);
            if (o == null) {
                continue;
            }
            Function f = fm.getFunctionAt(toAddr(t.addr));
            if (f == null) {
                continue;
            }
            if (o.get("new_name").getAsString().equals(f.getName())
                    && o.get("old_name").getAsString().equals(t.label)) {
                jsonlIdentityOk++;
            } else {
                jsonlProblems.add(hex(t.addr) + " identity old/new mismatch");
            }
            JsonArray ea = o.getAsJsonArray("actions");
            boolean actsOk = ea != null && ea.size() == t.acts.size();
            if (actsOk) {
                for (int i = 0; i < t.acts.size(); i++) {
                    JsonObject ao = ea.get(i).getAsJsonObject();
                    Act a = t.acts.get(i);
                    if (ao.get("id").getAsInt() != a.id
                            || !ao.get("name").getAsString().equals(a.name)
                            || !ao.has("args")
                            || !ao.get("args").getAsString().equals(a.args == null ? "" : a.args)) {
                        actsOk = false;
                        break;
                    }
                }
            }
            if (actsOk) {
                jsonlActionsOk++;
            } else {
                jsonlProblems.add(hex(t.addr) + " action list mismatch");
            }
            int size = (int) f.getBody().getNumAddresses();
            int ic = countInstructions(f.getBody());
            if (o.get("size").getAsInt() == size && o.get("instructions").getAsInt() == ic) {
                jsonlMetadataOk++;
            } else {
                jsonlProblems.add(hex(t.addr) + " size/instruction mismatch");
            }
            if (open) {
                String code = null;
                try {
                    DecompileResults res = di.decompileFunction(f, 120, monitor);
                    if (res != null && res.decompileCompleted() && res.getDecompiledFunction() != null) {
                        code = res.getDecompiledFunction().getC();
                    }
                } catch (Exception ex) {
                    code = null;
                }
                if (code != null && !code.trim().isEmpty()) {
                    redecompiled++;
                    String h = sha256(code);
                    if (h.equals(o.get("pseudocode_sha256").getAsString())) {
                        pseudocodeMatch++;
                    } else {
                        jsonlProblems.add(hex(t.addr) + " pseudocode hash mismatch");
                    }
                } else {
                    decompileFailed++;
                    jsonlProblems.add(hex(t.addr) + " redecompile failed");
                }
            }
        }
        if (open) {
            di.dispose();
        }
        check(checks, "renamed_jsonl_125_rows", jsonlRows == 125 && jsonlSetOk,
                "rows=" + jsonlRows + " entries_match=" + jsonlSetOk);
        check(checks, "renamed_jsonl_identity_actions_metadata",
                jsonlIdentityOk == 125 && jsonlActionsOk == 125 && jsonlMetadataOk == 125,
                "identity=" + jsonlIdentityOk + " actions=" + jsonlActionsOk
                        + " metadata=" + jsonlMetadataOk + "/125");
        check(checks, "redecompiled_pseudocode_hashes_match",
                redecompiled == 125 && pseudocodeMatch == 125 && decompileFailed == 0,
                "redecompiled=" + redecompiled + " match=" + pseudocodeMatch
                        + " failed=" + decompileFailed);

        // Surface any cross-check failures.
        JsonArray jsonlProblemsJson = new JsonArray();
        for (int i = 0; i < jsonlProblems.size() && i < 100; i++) {
            jsonlProblemsJson.add(jsonlProblems.get(i));
        }

        boolean allPass = true;
        for (JsonElement e : checks) {
            if (!e.getAsJsonObject().get("pass").getAsBoolean()) {
                allPass = false;
            }
        }

        gaps.add("Entry-level coverage only: handler bodies were not executed; dispatch/runtime behavior remains unobserved.");
        gaps.add("Naming uses the lowest numeric action id as the primary id for shared handlers; all shared ids and names are in the plate comment and coverage target rows.");
        gaps.add("The 440 LAB_ targets were named/commented by the prior handler pass; lane 07 renamed only the 125 pre-existing FUN_ targets.");
        gaps.add("External (imported) functions are counted but not action handlers and were not renamed.");
        if (!jsonlProblems.isEmpty()) {
            gaps.add("JSONL cross-check issues: " + jsonlProblems.size() + " (see verification.json).");
        }

        JsonObject coverage = new JsonObject();
        coverage.addProperty("schema_version", 1);
        coverage.addProperty("generated_at", Instant.now().toString());
        coverage.addProperty("lane", "07-ghidra-rename");
        JsonObject inputs = new JsonObject();
        inputs.addProperty("action_table", tablePath);
        inputs.addProperty("action_table_sha256", sha256File(tablePath));
        inputs.addProperty("baseline_functions", baselinePath);
        inputs.addProperty("baseline_functions_sha256", sha256File(baselinePath));
        inputs.addProperty("renamed_functions", jsonlPath);
        inputs.addProperty("renamed_functions_sha256", sha256File(jsonlPath));
        if (applySummaryPath != null) {
            inputs.addProperty("apply_summary", applySummaryPath);
            inputs.addProperty("apply_summary_sha256", sha256File(applySummaryPath));
        }
        inputs.addProperty("project_copy", "artifacts/lanes/07-ghidra-rename/project/kotor.gpr");
        inputs.addProperty("program", currentProgram.getName());
        inputs.addProperty("program_sha256", currentProgram.getExecutableSHA256());
        inputs.addProperty("ghidra", "12.1.4 PUBLIC");
        inputs.addProperty("jdk", "Temurin 21.0.12.1+1");
        inputs.addProperty("run_mode", "analyzeHeadless -process swkotor.exe -noanalysis");
        coverage.add("inputs", inputs);

        JsonObject dispatcher = new JsonObject();
        dispatcher.addProperty("entry", hex(dispatchAddr));
        dispatcher.addProperty("name", df == null ? "" : df.getName());
        dispatcher.addProperty("present", dispatcherPresent);
        dispatcher.addProperty("named_correctly", dispatcherNamed);
        dispatcher.addProperty("plate_comment_present", dcomment != null);
        dispatcher.addProperty("plate_comment_ok", dispatcherCommentOk);
        coverage.add("dispatcher", dispatcher);

        JsonObject counts = new JsonObject();
        counts.addProperty("actions", actions.size());
        counts.addProperty("unique_targets", targets.size());
        counts.addProperty("lab_targets_already_named", labTargets);
        counts.addProperty("fun_targets_renamed", funTargets);
        counts.addProperty("targets_with_functions", targetsWithFunction);
        counts.addProperty("targets_named_expected", targetsNameExpected);
        counts.addProperty("unique_target_names", targetNames.size());
        counts.addProperty("target_comments_ok", targetsCommentOk);
        counts.addProperty("action_mappings_verified", actionsMapped);
        counts.addProperty("total_functions", totalFunctions);
        counts.addProperty("internal_functions", internalFunctions);
        counts.addProperty("external_functions", externalFunctions);
        counts.addProperty("baseline_functions", baselineRows);
        counts.addProperty("baseline_present", baselinePresent);
        counts.addProperty("baseline_missing", baselineRows - baselinePresent);
        counts.addProperty("baseline_non_handler_names_unchanged", baselineNameUnchanged);
        counts.addProperty("baseline_handler_targets_renamed", baselineHandlersSkipped);
        counts.addProperty("renamed_jsonl_rows", jsonlRows);
        counts.addProperty("renamed_jsonl_identity_ok", jsonlIdentityOk);
        counts.addProperty("renamed_jsonl_actions_ok", jsonlActionsOk);
        counts.addProperty("renamed_jsonl_metadata_ok", jsonlMetadataOk);
        counts.addProperty("redecompiled", redecompiled);
        counts.addProperty("pseudocode_hash_match", pseudocodeMatch);
        counts.addProperty("decompile_failed", decompileFailed);
        coverage.add("counts", counts);

        coverage.add("validation", checks);
        coverage.addProperty("all_checks_pass", allPass);
        coverage.add("coverage_gaps", gaps);
        JsonArray problems = new JsonArray();
        for (String s : missingTargets) {
            if (problems.size() >= 100) {
                break;
            }
            problems.add("missing target " + s);
        }
        for (String s : nameProblems) {
            if (problems.size() >= 100) {
                break;
            }
            problems.add("name " + s);
        }
        for (String s : commentProblems) {
            if (problems.size() >= 100) {
                break;
            }
            problems.add("comment " + s);
        }
        for (String s : baselineMissing) {
            if (problems.size() >= 100) {
                break;
            }
            problems.add("baseline missing " + s);
        }
        for (String s : baselineRenamed) {
            if (problems.size() >= 100) {
                break;
            }
            problems.add("baseline renamed " + s);
        }
        coverage.add("problems", problems);
        coverage.add("targets", coverageTargets);

        try (PrintWriter out = new PrintWriter(coveragePath, "UTF-8")) {
            out.println(new GsonBuilder().setPrettyPrinting().create().toJson(coverage));
        }

        JsonObject verification = new JsonObject();
        verification.addProperty("program", currentProgram.getName());
        verification.addProperty("program_sha256", currentProgram.getExecutableSHA256());
        verification.addProperty("program_copy", "artifacts/lanes/07-ghidra-rename/project");
        verification.addProperty("actions", actions.size());
        verification.addProperty("unique_targets", targets.size());
        verification.addProperty("fun_targets_renamed", funTargets);
        verification.addProperty("lab_targets_preexisting", labTargets);
        verification.addProperty("targets_with_functions", targetsWithFunction);
        verification.addProperty("targets_named_expected", targetsNameExpected);
        verification.addProperty("target_comments_ok", targetsCommentOk);
        verification.addProperty("action_mappings", actionsMapped);
        verification.addProperty("total_functions", totalFunctions);
        verification.addProperty("internal_functions", internalFunctions);
        verification.addProperty("external_functions", externalFunctions);
        verification.addProperty("baseline_functions", baselineRows);
        verification.addProperty("baseline_present", baselinePresent);
        verification.addProperty("baseline_non_handler_names_unchanged", baselineNameUnchanged);
        verification.addProperty("jsonl_rows", jsonlRows);
        verification.addProperty("redecompiled", redecompiled);
        verification.addProperty("pseudocode_hash_match", pseudocodeMatch);
        verification.addProperty("dispatcher_present", dispatcherPresent);
        verification.addProperty("dispatcher_named", dispatcherNamed);
        verification.addProperty("all_checks_pass", allPass);
        verification.add("checks", checks);
        verification.add("jsonl_problems", jsonlProblemsJson);
        try (PrintWriter out = new PrintWriter(verificationPath, "UTF-8")) {
            out.println(new GsonBuilder().setPrettyPrinting().create().toJson(verification));
        }

        println("VERIFY actions=" + actions.size() + " targets=" + targets.size()
                + " named=" + targetsNameExpected + " functions=" + targetsWithFunction
                + " comments=" + targetsCommentOk + " actions_mapped=" + actionsMapped);
        println("VERIFY functions total=" + totalFunctions + " internal=" + internalFunctions
                + " external=" + externalFunctions + " baseline=" + baselineRows
                + " present=" + baselinePresent);
        println("VERIFY renamed_jsonl=" + jsonlRows + " redecompiled=" + redecompiled
                + " hash_match=" + pseudocodeMatch + " jsonl_problems=" + jsonlProblems.size());
        println("VERIFY dispatcher present=" + dispatcherPresent + " named=" + dispatcherNamed);
        println("ALL_CHECKS_PASS=" + allPass);
        println("COVERAGE=" + coveragePath);
        println("VERIFICATION=" + verificationPath);
    }
}
