"""Independent NCS (NWScript) VM core for the KotOR 1.0.3.0 corpus.

The VM executes the documented NCS V1.0 stack machine with typed 4-byte
cells, a separate JSR/RETN return stack, SAVEBP/RESTOREBP globals, and
mediated ACTION calls.  It deliberately does not call engine code: every
action returns a typed default so the shipped corpus can be executed and
classified without the original executable.
"""
