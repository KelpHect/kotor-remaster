# License proposal (for the owner's decision)

Status: **proposal, nothing selected.** No `LICENSE` file exists and none is added by this
document. Until the owner chooses, no license is granted (see README). Not legal advice.

Scope: project-owned code and documentation. Third-party code keeps its own license
([`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md)). No license here can grant rights in
the game's data, names or marks.

## Options

| | MIT | Apache-2.0 | MPL-2.0 | GPL-3.0-or-later | Stay unlicensed |
|---|---|---|---|---|---|
| Others may reuse the code in closed products | Yes | Yes | Yes, but changes to our files stay open | No | No reuse at all |
| Explicit patent grant | No | Yes, ends for a party that sues over patents | Yes | Yes | n/a |
| Duties for redistributors | Keep copyright and license text | Keep license and `NOTICE`, state changes | Keep license, publish source of changed files | Publish full corresponding source | n/a |
| Fits current dependencies (zlib, MIT, Apache-2.0 OR MIT, public domain) | Yes | Yes | Yes | Yes | n/a |
| Binary shipped with the GPL FFmpeg build (review finding F3) | Possible; that binary falls under GPL terms | Possible with GPLv3; that binary falls under GPL terms | Possible; that binary falls under GPL terms | Native fit | Not distributable |
| Binary shipped with an LGPL FFmpeg build, or none | No extra effect | No extra effect | No extra effect | No extra effect | Not distributable |
| Outside contributions | Simple with DCO sign-off | Simple; section 5 covers contributions | Simple | Simple; later relicensing needs every contributor | Cannot be accepted safely |
| Later relicensing by the owner | Easy | Easy | Needs contributor consent | Needs contributor consent | Free while single-author |
| Used by comparable engine reimplementations | Some | Some | Few | Most (xoreos, reone, OpenMW, ScummVM) | n/a |

## What each choice changes

- **MIT**: shortest text and fewest duties. Anyone, including a commercial party, can take the
  engine closed. No patent language.
- **Apache-2.0**: same freedom as MIT plus an explicit patent grant and a `NOTICE` mechanism that
  matches the attribution file this repository now has. Not compatible with GPLv2-only code.
- **MPL-2.0**: file-level copyleft. Changes to the engine's own files must be published; the
  files can still be combined with other code.
- **GPL-3.0-or-later**: every distributed derivative stays open, and GPL code such as the GPL
  FFmpeg build can be linked with no special handling. It does not make copying from GPL
  reimplementations acceptable while the clean-room rule stands: that rule is about provenance,
  not license.
- **Unlicensed**: keeps every option open while the release blockers in `LEGAL_REVIEW.md` are
  worked, but nobody else may legally use, build on or contribute to the code.

## Recommendation

Apache-2.0 if the owner wants the widest reuse and a patent grant; GPL-3.0-or-later if the
owner wants forks to stay open. The earlier review text names MIT/Apache-2.0 as the working
assumption, so Apache-2.0 is the default this proposal puts forward. Whichever is chosen, keep
the repository unlicensed until release blockers R1, R4 and R5 are closed: a license cannot be
withdrawn from copies already taken.

## Decisions for the owner

1. The license: MIT, Apache-2.0, MPL-2.0, GPL-3.0-or-later, or stay unlicensed for now.
2. FFmpeg in binaries: none, an LGPL build, or the GPL build (R3).
3. Contribution terms: DCO sign-off (suggested) or a CLA.
4. Whether documentation and evidence use the code license or CC BY 4.0.

## After the decision

Add `LICENSE` at the root, update the README badge and "Legal and scope", add contribution
terms in a `CONTRIBUTING.md`, and for Apache-2.0 add a root `NOTICE`.
