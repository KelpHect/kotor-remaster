# Recon 54 - lip sync from LIP tracks and the talk clip

Status: corrected. The first version of this note said the shipped heads carry no per-shape
clips, so the mouth had to be procedural, and the engine drove invented jaw rotations. That was
wrong: the shapes live in one clip, `talk`. Target install:
`0290889e0cea6ebc00021c1bdb945704fc4c45afaf7fcebabb0e82ba674437e6`.

## Shipped data

- `lips/*_loc.mod` hold 18206 LIP resources. All of them are `LIP V1.0`, a 16-byte header (magic,
  float duration, u32 count) and 5-byte keys (float seconds, u8 shape). Every first key is at 0,
  every last key at the duration, and the shapes are 0..15.
- `visemes.2da` names the 16 shapes (`Silence`, `Eat`, `If`, `Ox`, `Oat`, `Wet`, `Size`, `Church`,
  `Fave`, `Though`, `Told`, `Bump`, `New`, `Roar`, `Cage`, `Earth`). Its `animname` column is empty
  and the name sits in the first data column, which `FacialRig::load` allows for.
- The `talk` clip (on `S_Male02`, reached through a head's supermodel chain) lasts 0.5 s and has 16
  keys, 1/30 s apart. It animates only `f_um_g`, `f_lmc_g`, `f_rmc_g`, `f_jaw_g`, `f_Rlm_g`,
  `f_Llm_g` and `f_tonguetip_g`.
- Head models name `neck_g` as their head root in the model header (file offset 0xB4). A head has
  its own `pause1`, with the blink, eye and neck motion.

## Retail (traced in the executable)

- `00616310` loads the LIP and turns each key into a normalized time and a value of
  (shape + 1) / 16.
- `006994a0` and `0069d540` forward that track, with the clip name `talk`, to the body and head
  models. `00485130` resolves the clip through the supermodel chain and turns each value into a
  clip time.
- `00486670` finds the two keys around the channel's progress and has `00483d80` blend the clip's
  pose at those two times. So a shape is a pose at one moment of `talk`, and between keys the mouth
  blends two poses rather than sweeping through the clip. After the last key the pose fades over
  half a second.
- `006993b0` plays a clip on the body and on the head under the same name.

## Engine

`FacialRig::facePose` gives the two clip fractions, the blend and the fade weight for a moment of
a track. `GameSession::updateFacialMeshes` puts that on the line's speaker (the entry's Speaker
object, or the stand-in of a cutscene conversation). `Mdl::poseWorld` samples `talk` twice and
mixes them over the nodes the clip animates. `posedHeadTriangles` poses a separate head with its
own clip and the track, on the body's hook as the body is posed this frame, blend included.

## Inferred or chosen here

- Keeping a head's clips to its head root and below is inferred from the header field; the routine
  that would state it was not traced to that detail.
- The track clock uses the real frame time rather than the simulation's capped one, so a long frame
  cannot put the mouth behind the voice. Retail's channel clock is game time; this is a choice.
- Whether a body talk clip (`tlknorm` and its neighbours, ids 0x2736..0x273a) plays by default for
  each line is not determined. Their callers are `00611700`, `00611850`, `006306e0`, `006313a0`,
  `0063e930` and `006655e0`. The engine plays none unless a script or the conversation asks.
