# Historical nwnnsscomp rebuild

Sprint 3's last three compiler-limited scripts (`docs/recon/38_BODY_ORDER.md`) need the
*historical* function-address assignment used by the shipped BioWare compiler. The shipped
`nwnnsscomp_ktool.exe` discovers callees in call arguments from left to right; the historical
compiler discovered them from right to left. For scripts whose call arguments contain two
function calls, that changes which body is emitted first while every instruction stays the same.

This directory records the small, source-level compiler patch used to reproduce that behavior,
plus the build-only changes needed to compile the 2003 sources with a modern MSVC.

## Patch contents

`nwnnsscomp-historical.patch` (against `virusman/nwnexplorer` commit
`ead0f1762f5df9b88d5a0eb2726d68d457e80e48`, the `nwntools` sources):

1. `_NscLib/NscCodeGenerator.cpp`, `.h`: add `GatherUsedArguments`, which walks a call's
   argument records right-to-left before adding callees. This is the historical address
   assignment; code generation is untouched.
2. `_NwnLib/NwnStdLoader.cpp`, `nwnnsscomp/nwnnsscomp.cpp`: build-only changes. Replace the
   `CRegKey` ATL dependency with the Win32 registry API and allow compile-only operation
   without a Neverwinter Nights install (`chitin.key` absent, includes supplied with `-i`).

The patch changes no opcode selection, expression codegen, or stack layout.

## Build

```powershell
git clone https://github.com/virusman/nwnexplorer C:\dev-overflow\nwntools
git -C C:\dev-overflow\nwntools checkout ead0f1762f5df9b88d5a0eb2726d68d457e80e48
git -C C:\dev-overflow\nwntools apply <repo>\tools\compiler\nwnnsscomp-historical.patch
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe' `
  C:\dev-overflow\nwntools\nwnnsscomp\nwnnsscomp.vcxproj `
  /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143 /m /v:m
```

The result is `C:\dev-overflow\nwntools\nwnnsscomp\Release\nwnnsscomp.exe`
(SHA-256 of the build used for Sprint 3:
`676e56379609ce19a2b76781b402654df2b521bc938766760d3a322c530e7424`).

## Use

KotOR-era scripts compile with the pre-1.30 rules, so pass `-v129`; the CLI also needs the
include directory:

```powershell
nwnnsscomp.exe -c -i . -v129 script.nss
```

This compiler is used only for the three body-order scripts and the restored dead block of
`k_plev_space_hb`. The other 8,662 scripts use
`nwnnsscomp_ktool.exe`, whose KotOR group-slice struct codegen is not present in the stock
nwntools sources.
