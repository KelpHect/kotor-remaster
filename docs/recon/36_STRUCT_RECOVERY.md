# 36 — Source-level struct recovery for the 201 group-slice files

Status: **struct recovery complete** - 201/201 struct globals recovered from the shipped bytecode; 2/201 are fully raw byte-identical and the remaining 199 differ only by one five-instruction compiler cleanup block.

| check | result |
|---|---|
| Files processed | **201/201** |
| Structs recovered | **201/201** |
| Source-level byte-identical | **2/201** |
| Cleanup-only residuals | **199** |

The remaining residuals are short one five-instruction compiler cleanup block (`CPTOPSP; CPDOWNSP; MOVSP; JMP; MOVSP`); that is the decompiler-structuring/MOVSP residual described in `docs/recon/31_COMPILER_CODEGEN.md`. 187 of the 199 are completed by the cleanup-only ABI insert; 12 use the full struct-group path because their cleanup shape differs.
