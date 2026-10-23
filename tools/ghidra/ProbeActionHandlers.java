// Probe: verify the copied project opens and inspect action handler targets.
// Read-only; used before DefineActionHandlers.java.
// @category KotOR

import java.io.FileReader;
import java.util.Map;
import java.util.TreeMap;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class ProbeActionHandlers extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("usage: ProbeActionHandlers.java <action_table.json>");
            return;
        }
        println("PROGRAM=" + currentProgram.getName());
        println("EXECUTABLE_PATH=" + currentProgram.getExecutablePath());
        println("IMAGE_BASE=" + currentProgram.getImageBase());
        println("LANGUAGE=" + currentProgram.getLanguageID());
        println("FUNCTIONS=" + currentProgram.getFunctionManager().getFunctionCount());

        JsonObject root = JsonParser.parseReader(new FileReader(args[0])).getAsJsonObject();
        JsonArray actions = root.getAsJsonArray("actions");
        TreeMap<Long, String> labs = new TreeMap<>();
        for (JsonElement e : actions) {
            JsonObject a = e.getAsJsonObject();
            String h = a.get("handler").getAsString();
            if (h.startsWith("LAB_")) {
                labs.put(Long.parseLong(h.substring(4), 16), h);
            }
        }
        println("LAB_UNIQUE=" + labs.size() + " FIRST=" + labs.firstKey() + " LAST=" + labs.lastKey());

        String dispatch = root.getAsJsonObject("dispatch").get("entry").getAsString();
        long dva = Long.parseLong(dispatch, 16);
        Address da = toAddr(dva);
        Function df = currentProgram.getFunctionManager().getFunctionAt(da);
        Function dc = currentProgram.getFunctionManager().getFunctionContaining(da);
        println("DISPATCHER=" + da + " at=" + (df == null ? "none" : df.getName())
                + " containing=" + (dc == null ? "none" : dc.getName() + "@" + dc.getEntryPoint()));

        int n = 0;
        for (Map.Entry<Long, String> e : labs.entrySet()) {
            if (n >= 6) {
                break;
            }
            Address a = toAddr(e.getKey());
            Function f = currentProgram.getFunctionManager().getFunctionAt(a);
            Function c = currentProgram.getFunctionManager().getFunctionContaining(a);
            Instruction ins = currentProgram.getListing().getInstructionAt(a);
            boolean dis = false;
            if (ins == null) {
                disassemble(a);
                ins = currentProgram.getListing().getInstructionAt(a);
                dis = true;
            }
            int callRefs = 0;
            int anyRefs = 0;
            ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(a);
            while (ri.hasNext()) {
                Reference r = ri.next();
                anyRefs++;
                if (r.getReferenceType().isCall()) {
                    callRefs++;
                }
            }
            StringBuilder b = new StringBuilder();
            if (ins != null) {
                Instruction t = ins;
                for (int i = 0; i < 4 && t != null; i++) {
                    b.append(t.toString()).append(" ; ");
                    t = t.getNext();
                }
            }
            println(e.getValue() + " funcAt=" + (f == null ? "-" : f.getName())
                    + " containing=" + (c == null ? "-" : c.getName() + "@" + c.getEntryPoint())
                    + " ins=" + (ins == null ? "NULL" : b.toString())
                    + " anyRefs=" + anyRefs + " callRefs=" + callRefs + " disassembled=" + dis);

            if (n == 0) {
                println("TEST createFunction at " + a);
                try {
                    CreateFunctionCmd cmd = new CreateFunctionCmd("PROBE_ONLY", a, null, SourceType.USER_DEFINED);
                    cmd.applyTo(currentProgram, monitor);
                    Function created = cmd.getFunction();
                    println("TEST created=" + (created == null ? "null" : created.getName())
                            + " bodyAddrs=" + (created == null ? -1 : created.getBody().getNumAddresses()));
                    if (created != null) {
                        int ic = 0;
                        ghidra.program.model.listing.InstructionIterator it =
                                currentProgram.getListing().getInstructions(created.getBody(), true);
                        while (it.hasNext()) {
                            it.next();
                            ic++;
                        }
                        println("TEST bodyInstructions=" + ic + " last=" + created.getBody().getMaxAddress());
                    }
                } catch (Exception ex) {
                    println("TEST createFunction exception: " + ex);
                }
            }
            n++;
        }
        println("PROBE_DONE");
    }
}
