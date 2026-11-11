#pragma once

namespace ke {

class GameSession;

// --family-test textureaudit: writes, under KE_TEX_AUDIT_DIR, every texture of the install and the
// draw state the engine's own functions choose for every material that names one.
int runTextureAudit(GameSession& game);
// --family-test materials: blend, depth, cube, water and flare state from shipped data.
int runMaterialFamilyTest(GameSession& game);

} // namespace ke
