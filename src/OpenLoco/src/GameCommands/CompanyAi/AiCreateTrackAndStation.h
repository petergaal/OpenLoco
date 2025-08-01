#include "GameCommands/GameCommands.h"

namespace OpenLoco::GameCommands
{
    struct AiTrackAndStationPlacementArgs
    {
        static constexpr auto command = GameCommand::aiCreateTrackAndStation;

        AiTrackAndStationPlacementArgs() = default;
        explicit AiTrackAndStationPlacementArgs(const registers& regs)
            : pos(regs.ax, regs.cx, regs.di)
            , rotation(regs.bh & 0x3)
            , trackObjectId(regs.dl)
            , stationObjectId(regs.dh)
            , stationLength((regs.edi >> 24) & 0xFFU)
            , mods((regs.edi >> 16) & 0xFU)
            , unk1((regs.edx >> 16) & 0xFFU)
            , bridge((regs.edx >> 24) & 0xFFU)
        {
        }

        World::Pos3 pos;
        uint8_t rotation;
        uint8_t trackObjectId;
        uint8_t stationObjectId;
        uint8_t stationLength;
        uint8_t mods;
        uint8_t unk1;
        uint8_t bridge;

        explicit operator registers() const
        {
            registers regs;
            regs.ax = pos.x;
            regs.cx = pos.y;
            regs.bh = rotation;
            regs.edx = (trackObjectId & 0xFFU) | ((stationObjectId & 0xFFU) << 8) | ((unk1 & 0xFFU) << 16) | ((bridge & 0xFFU) << 24);
            regs.edi = (pos.z & 0xFFFFFU) | ((mods & 0xFU) << 16) | ((stationLength & 0xFFU) << 24);
            return regs;
        }
    };

    void aiCreateTrackAndStation(registers& regs);
}
