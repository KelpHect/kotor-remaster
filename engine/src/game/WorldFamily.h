#pragma once

namespace ke {

class GameSession;

// --family-test world3d: scene data behind the 3D world.
int runWorldFamilyTest(GameSession& game);
// --family-test worldtextures: every texture lookup that fails, over every shipped module.
int runWorldTextureAudit(GameSession& game);
// --family-test worldposes: how every posed body sits on the walkmesh, over every shipped module.
int runWorldPoseSurvey(GameSession& game);

} // namespace ke
