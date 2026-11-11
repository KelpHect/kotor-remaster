#pragma once

// The conversation panel: the shipped dialog.gui (LBL_MESSAGE for a line, LB_REPLIES for the
// player's choices) bound to the session's DialogueView. The application paints it over the
// world between two letterbox bars and routes its control actions here.

#include "game/GameSession.h"
#include "game/GuiPaint.h"
#include "game/GuiSystem.h"

#include <map>
#include <string>

namespace ke {

class DialogueScreen {
public:
    // `computer` is the terminal screen (computer.gui), used while the conversation's type is
    // Computer; null keeps every conversation on the dialog panel. `camera` is computercamera.gui,
    // the live feed a terminal switches to for a static-camera node (null: the terminal stays up).
    DialogueScreen(GuiSystem& gui, GameSession& game, GuiPainter* painter = nullptr,
                   GuiSystem* computer = nullptr, GuiSystem* camera = nullptr);

    // The screen the conversation is on now: the terminal for a Computer conversation, and
    // computercamera.gui while its live feed is up.
    GuiSystem& activeGui();
    bool computerShown() const;
    // CSWGuiDialogComputerCamera (ctor 006a95f0): a Computer conversation's node with CameraAngle 6
    // swaps the terminal for the placeable camera's view (CGuiInGame::SetComputerCameraShot 0062c030,
    // reached from 00631d80 and 00633660) with one label, LBL_RETURN. It stays while the node is
    // spoken or held; Enter (006a7230 input 0x27 / 0x2d) cancels it like skipping a line.
    bool cameraFeedShown() const;
    // Enter or a click: the CSWGuiDialog input handler's skip (004ae970). True when it ended a feed.
    bool cancelFeed();
    // 006a95f0: the panel takes the window's size and LBL_RETURN sits at ((W - w) / 2, H - 75).
    void fitFeed(int windowWidth, int windowHeight);

    // Once per frame while a conversation is up.
    void update();
    // A control action (tag). True when this class consumed it.
    bool action(const std::string& tag);
    // Design-space height of each letterbox bar (the panel's own extent in dialog.gui).
    static constexpr float kBarHeight = 100.0f;
    bool repliesShown() const { return repliesShown_; }
    // The spoken line over the replies, in the top bar (CSWGuiDialogTop, 006a78c0 / 006a7ae0).
    // `barTop` is the bottom of the top letterbox bar in window pixels.
    void paintTop(ImDrawList* draw, float width, float barTop);
    // 006a8b40 / 006a7ca0: the panel, LBL_MESSAGE and LB_REPLIES take the window's width less the
    // safe margins (00409ee0: 48 a side); dialog.gui's 544 is that for 640. Call with the window width.
    void fitWidth(int windowWidth);

private:
    GuiSystem& gui_;
    GameSession& game_;
    GuiPainter* painter_ = nullptr;
    GuiSystem* computer_ = nullptr;
    GuiSystem* camera_ = nullptr;
    int fittedFeedW_ = 0;
    int fittedFeedH_ = 0;
    bool feedLogged_ = false;
    void updateComputer();
    bool actionComputer(const std::string& tag);
    std::string shown_;
    std::map<std::string, std::string> itemTags_; // item resref to its UTI tag, lower case
    bool repliesShown_ = false;
    std::string topLine_; // the line shown in the top bar while the replies are up
    int fittedWidth_ = 0;
};

} // namespace ke
