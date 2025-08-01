#include "Audio/Audio.h"
#include "Config.h"
#include "Date.h"
#include "Economy/Economy.h"
#include "Effects/Effect.h"
#include "Effects/SmokeEffect.h"
#include "Entities/EntityManager.h"
#include "GameCommands/GameCommands.h"
#include "GameCommands/Vehicles/VehicleChangeRunningMode.h"
#include "GameCommands/Vehicles/VehicleSell.h"
#include "GameState.h"
#include "Graphics/Gfx.h"
#include "Localisation/FormatArguments.hpp"
#include "Localisation/Formatting.h"
#include "Localisation/StringIds.h"
#include "Map/RoadElement.h"
#include "Map/StationElement.h"
#include "Map/SurfaceElement.h"
#include "Map/TileManager.h"
#include "Map/Track/Track.h"
#include "Map/Track/TrackData.h"
#include "Map/TrackElement.h"
#include "MessageManager.h"
#include "Objects/AirportObject.h"
#include "Objects/CargoObject.h"
#include "Objects/DockObject.h"
#include "Objects/IndustryObject.h"
#include "Objects/ObjectManager.h"
#include "Objects/RoadObject.h"
#include "Objects/RoadStationObject.h"
#include "Objects/TrackObject.h"
#include "Objects/VehicleObject.h"
#include "OrderManager.h"
#include "Orders.h"
#include "Random.h"
#include "RoutingManager.h"
#include "ScenarioManager.h"
#include "SceneManager.h"
#include "Tutorial.h"
#include "Ui/WindowManager.h"
#include "Vehicle.h"
#include "VehicleManager.h"
#include "ViewportManager.h"
#include "World/CompanyManager.h"
#include "World/CompanyRecords.h"
#include "World/IndustryManager.h"
#include "World/StationManager.h"
#include "World/TownManager.h"
#include <OpenLoco/Interop/Interop.hpp>
#include <OpenLoco/Math/Bound.hpp>
#include <OpenLoco/Math/Trigonometry.hpp>
#include <cassert>
#include <numeric>
#include <optional>

using namespace OpenLoco::Interop;
using namespace OpenLoco::Literals;
using namespace OpenLoco::World;

namespace OpenLoco::Vehicles
{
    static loco_global<uint32_t, 0x011360D0> _vehicleUpdate_manhattanDistanceToStation;
    static loco_global<VehicleHead*, 0x01136118> _vehicleUpdate_head;
    static loco_global<Vehicle1*, 0x0113611C> _vehicleUpdate_1;
    static loco_global<Vehicle2*, 0x01136120> _vehicleUpdate_2;
    static loco_global<VehicleBogie*, 0x01136124> _vehicleUpdate_frontBogie;
    static loco_global<VehicleBogie*, 0x01136128> _vehicleUpdate_backBogie;
    static loco_global<int32_t, 0x0113612C> _vehicleUpdate_var_113612C; // Speed
    static loco_global<int32_t, 0x01136130> _vehicleUpdate_var_1136130; // Speed
    static loco_global<int16_t, 0x01136168> _vehicleUpdate_targetZ;
    static loco_global<uint16_t, 0x01136458> _1136458; // Actually just a bool
    static loco_global<Status, 0x0113646C> _vehicleUpdate_initialStatus;
    static loco_global<uint8_t, 0x0113646D> _vehicleUpdate_helicopterTargetYaw;
    static loco_global<AirportMovementNodeFlags, 0x00525BB0> _vehicleUpdate_helicopterAirportMovement;
    static loco_global<uint8_t[2], 0x0113601A> _113601A; // Track Connection mod global

    static constexpr uint16_t kTrainOneWaySignalTimeout = 1920;
    static constexpr uint16_t kTrainTwoWaySignalTimeout = 640;
    static constexpr uint16_t kBusSignalTimeout = 960;                // Time to wait before turning around at barriers
    static constexpr uint16_t kTramSignalTimeout = 2880;              // Time to wait before turning around at barriers
    static constexpr uint8_t kAiSellCrashedVehicleTimeout = 14;       // Number of days after a crash before selling
    static constexpr uint8_t kRestartStoppedRoadVehiclesTimeout = 20; // Number of days before stopped road vehicle (bus and tram) is restarted
    static constexpr uint16_t kReliabilityLossPerDay = 4;
    static constexpr uint16_t kReliabilityLossPerDayObsolete = 10;

    struct WaterPathingResult
    {
        World::Pos2 headTarget;

        StationId stationId; // If stationId is null, stationPosition is invalid
        World::Pos3 stationPos;

        constexpr WaterPathingResult(const World::Pos2 headTarget, const StationId stationId, const World::Pos3 stationPos)
            : headTarget(headTarget)
            , stationId(stationId)
            , stationPos(stationPos)
        {
        }
        explicit constexpr WaterPathingResult(const World::Pos2 headTarget)
            : headTarget(headTarget)
            , stationId(StationId::null)
            , stationPos({})
        {
        }
    };
    static WaterPathingResult waterPathfind(const VehicleHead& head);

    void VehicleHead::updateVehicle()
    {
        // TODO: Refactor to use the Vehicle super class
        VehicleBase* v = this;
        while (v != nullptr)
        {
            if (v->updateComponent())
            {
                break;
            }
            v = v->nextVehicleComponent();
        }
    }

    // 0x004A8B81
    bool VehicleHead::update()
    {
        Vehicle train(head);
        _vehicleUpdate_head = train.head;
        _vehicleUpdate_1 = train.veh1;
        _vehicleUpdate_2 = train.veh2;

        _vehicleUpdate_initialStatus = status;
        updateDrivingSounds();

        _vehicleUpdate_frontBogie = reinterpret_cast<VehicleBogie*>(0xFFFFFFFF);
        _vehicleUpdate_backBogie = reinterpret_cast<VehicleBogie*>(0xFFFFFFFF);

        Vehicle2* veh2 = _vehicleUpdate_2;
        _vehicleUpdate_var_113612C = veh2->currentSpeed.getRaw() >> 7;
        _vehicleUpdate_var_1136130 = veh2->currentSpeed.getRaw() >> 7;

        if (var_5C != 0)
        {
            var_5C--;
        }

        if (tileX == -1)
        {
            if (!train.cars.empty())
            {
                return false;
            }

            train.tail->trainDanglingTimeout++;
            if (train.tail->trainDanglingTimeout < 960)
            {
                return false;
            }
            VehicleManager::deleteTrain(*this);
            return false;
        }
        updateBreakdown();
        bool continueUpdating = true;
        switch (mode)
        {
            case TransportMode::rail:
            case TransportMode::road:
                continueUpdating = updateLand();
                break;
            case TransportMode::air:
                continueUpdating = updateAir();
                break;
            case TransportMode::water:
                continueUpdating = updateWater();
                break;
        }
        if (continueUpdating)
        {
            tryCreateInitialMovementSound();
        }
        return continueUpdating;
    }

    // 0x004C3C65
    void VehicleHead::updateMonthly()
    {
        Vehicle train(head);
        if ((tileX != -1) && !has38Flags(Flags38::isGhost))
        {
            constexpr ExpenditureType vehTypeToCost[] = {
                ExpenditureType::TrainRunningCosts,
                ExpenditureType::BusRunningCosts,
                ExpenditureType::TruckRunningCosts,
                ExpenditureType::TramRunningCosts,
                ExpenditureType::AircraftRunningCosts,
                ExpenditureType::ShipRunningCosts,
            };
            const auto costs = calculateRunningCost();
            CompanyManager::applyPaymentToCompany(owner, costs, vehTypeToCost[enumValue(vehicleType)]);

            const auto monthlyProfit = train.veh2->curMonthRevenue - costs;

            auto& profitHist = train.veh2->profit;
            std::rotate(std::begin(profitHist), std::end(profitHist) - 1, std::end(profitHist));
            profitHist[0] = monthlyProfit;

            train.veh2->curMonthRevenue = 0;
            Ui::WindowManager::invalidate(Ui::WindowType::vehicle, enumValue(id));
        }

        for (auto& car : train.cars)
        {
            // Reduce the refund cost by 0.78% each month
            car.front->refundCost -= car.front->refundCost / 128;
        }

        calculateRefundCost();
    }

    // 0x004B8340
    static void recalculateTrainMinReliability(VehicleHead& head)
    {
        Vehicle train(head);
        const auto getReliability = [](const Car& car) -> uint16_t {
            if (car.front->reliability == 0)
            {
                return 0xFFFFU;
            }
            return car.front->reliability;
        };

        const auto reliabilityPredicate = [&getReliability](const Car& carLhs, const Car& carRhs) {
            return getReliability(carLhs) < getReliability(carRhs);
        };

        auto minReliability = 0xFFFFU;
        if (!train.cars.empty())
        {
            minReliability = (*std::min_element(train.cars.begin(), train.cars.end(), reliabilityPredicate)).front->reliability;
        }
        train.veh2->reliability = minReliability == 0xFFFFU ? 0 : minReliability / 256;
    }

    // 0x004B9509
    void VehicleHead::updateDaily()
    {
        bool resetStoppedTimeout = true;
        if (status == Status::stopped)
        {
            if (mode == TransportMode::road)
            {
                if (tileX != -1
                    && hasVehicleFlags(VehicleFlags::commandStop))
                {
                    if (Tutorial::state() == Tutorial::State::none)
                    {
                        restartStoppedCarsTimeout++;
                        if (restartStoppedCarsTimeout >= kRestartStoppedRoadVehiclesTimeout)
                        {
                            GameCommands::VehicleChangeRunningModeArgs args{};
                            args.head = head;
                            args.mode = GameCommands::VehicleChangeRunningModeArgs::Mode::startVehicle;
                            auto regs = static_cast<Interop::registers>(args);
                            regs.bl = GameCommands::Flags::apply;
                            GameCommands::vehicleChangeRunningMode(regs);
                            if (static_cast<uint32_t>(regs.ebx) == GameCommands::FAILURE)
                            {
                                liftUpVehicle();
                            }
                        }
                        else
                        {
                            resetStoppedTimeout = false;
                        }
                    }
                    else
                    {
                        resetStoppedTimeout = false;
                    }
                }
            }
            else
            {
                resetStoppedTimeout = false;
            }
        }
        if (resetStoppedTimeout)
        {
            restartStoppedCarsTimeout = 0;
        }

        if (status == Status::crashed || status == Status::stuck)
        {
            crashedTimeout = Math::Bound::add(crashedTimeout, 1U);
            if (!CompanyManager::isPlayerCompany(owner))
            {
                if (crashedTimeout >= kAiSellCrashedVehicleTimeout && aiThoughtId != 0xFF)
                {
                    auto* aiCompany = CompanyManager::get(owner);
                    auto& aiThought = aiCompany->aiThoughts[aiThoughtId];
                    const auto toBeRemovedId = id;
                    sub_4AD778();
                    status = Status::stopped;
                    Vehicle train(*this);
                    train.veh2->currentSpeed = 0_mph;

                    auto stashOwner = GameCommands::getUpdatingCompanyId();
                    GameCommands::setUpdatingCompanyId(owner);

                    GameCommands::VehicleSellArgs args{};
                    args.car = toBeRemovedId;
                    GameCommands::doCommand(args, GameCommands::Flags::apply);
                    // 'this' pointer is invalid at this point!
                    GameCommands::setUpdatingCompanyId(stashOwner);
                    removeEntityFromThought(aiThought, toBeRemovedId);
                    return;
                }
            }
        }

        Vehicle train(*this);
        for (auto& car : train.cars)
        {
            // Bit of overkill iterating the whole car
            // as only the first carComponent has cargo
            // but matches vanilla. Remove when confirmed
            // not used.
            for (auto& carComponent : car)
            {
                if (carComponent.front->secondaryCargo.qty != 0)
                {
                    carComponent.front->secondaryCargo.numDays = Math::Bound::add(carComponent.front->secondaryCargo.numDays, 1U);
                }
                // Bit of overkill as back doesn't hold cargo but
                // it matches vanilla. Remove when confirmed not used
                if (carComponent.back->secondaryCargo.qty != 0)
                {
                    carComponent.back->secondaryCargo.numDays = Math::Bound::add(carComponent.back->secondaryCargo.numDays, 1U);
                }
                if (carComponent.body->primaryCargo.qty != 0)
                {
                    carComponent.body->primaryCargo.numDays = Math::Bound::add(carComponent.body->primaryCargo.numDays, 1U);
                }
            }
        }

        for (auto& car : train.cars)
        {
            auto& front = *car.front;
            if (front.reliability == 0)
            {
                continue;
            }

            if (front.hasBreakdownFlags(BreakdownFlags::brokenDown))
            {
                front.breakdownTimeout--;
                if (front.breakdownTimeout == 0)
                {
                    front.breakdownFlags &= ~BreakdownFlags::brokenDown;
                    applyBreakdownToTrain();
                }
            }
            else if (front.timeoutToBreakdown != 0xFFFFU)
            {
                front.timeoutToBreakdown = Math::Bound::sub(front.timeoutToBreakdown, 1U);

                if (front.timeoutToBreakdown == 0)
                {
                    sub_4BA873(front);
                    front.breakdownFlags |= BreakdownFlags::breakdownPending;
                }
            }
            auto newReliability = front.reliability;
            auto* vehObj = ObjectManager::get<VehicleObject>(front.objectId);
            newReliability -= vehObj->obsolete <= getCurrentYear() ? kReliabilityLossPerDayObsolete : kReliabilityLossPerDay;
            newReliability = std::max<uint16_t>(newReliability, 100);
            front.reliability = newReliability;
        }
        recalculateTrainMinReliability(*this);
    }

    // 0x004BA8D4
    void VehicleHead::updateBreakdown()
    {
        switch (status)
        {
            case Status::unk_0:
            case Status::stopped:
            case Status::waitingAtSignal:
            case Status::unloading:
            case Status::loading:
            case Status::crashed:
            case Status::stuck:
                return;
            case Status::travelling:
            case Status::approaching:
            case Status::brokenDown:
            case Status::landing:
            case Status::taxiing1:
            case Status::taxiing2:
            case Status::takingOff:
                break;
        }
        Vehicle train(head);
        for (auto& car : train.cars)
        {
            if (car.front->hasBreakdownFlags(BreakdownFlags::brokenDown))
            {
                if ((ScenarioManager::getScenarioTicks() & 3) == 0)
                {
                    auto v2 = car.body; // body

                    Smoke::create(v2->position + World::Pos3{ 0, 0, 4 });
                }
            }

            if (car.front->hasBreakdownFlags(BreakdownFlags::breakdownPending) && !SceneManager::isTitleMode())
            {
                auto newConfig = Config::get();
                if (!newConfig.breakdownsDisabled)
                {
                    car.front->breakdownFlags &= ~BreakdownFlags::breakdownPending;
                    car.front->breakdownFlags |= BreakdownFlags::brokenDown;
                    car.front->breakdownTimeout = 5;
                    applyBreakdownToTrain();

                    auto soundId = (Audio::SoundId)gPrng1().randNext(26, 26 + 5);
                    Audio::playSound(soundId, car.body->position + World::Pos3{ 0, 0, 22 });
                }
            }
        }
    }

    // 0x004BAA76
    void VehicleHead::applyBreakdownToTrain()
    {
        Vehicle train(head);
        bool isBrokenDown = false;
        bool trainStillPowered = false;
        // Check only the first bogie on each car for breakdown flags
        for (const auto& car : train.cars)
        {
            auto* vehObj = ObjectManager::get<VehicleObject>(car.front->objectId);
            if (vehObj == nullptr)
            {
                continue;
            }
            // Unpowered vehicles can not breakdown
            if (vehObj->power == 0)
            {
                continue;
            }
            if (car.front->hasBreakdownFlags(BreakdownFlags::brokenDown))
            {
                isBrokenDown = true;
            }
            else
            {
                trainStillPowered = true;
            }
        }
        if (isBrokenDown)
        {
            if (trainStillPowered)
            {
                train.veh2->var_73 |= Flags73::isBrokenDown | Flags73::isStillPowered;
            }
            else
            {
                train.veh2->var_73 |= Flags73::isBrokenDown;
                train.veh2->var_73 &= ~Flags73::isStillPowered;
            }
        }
        else
        {
            train.veh2->var_73 &= ~(Flags73::isBrokenDown | Flags73::isStillPowered);
        }
    }

    // 0x004AF7A4
    void VehicleHead::sub_4AF7A4()
    {
        registers regs{};
        regs.esi = X86Pointer(this);
        call(0x004AF7A4, regs);
    }

    // 0x004B90F0
    // eax : newVehicleTypeId
    // ebx : sourceVehicleTypeId;
    bool canVehiclesCouple(const uint16_t newVehicleTypeId, const uint16_t sourceVehicleTypeId)
    {
        auto newObject = ObjectManager::get<VehicleObject>(newVehicleTypeId);       // edi
        auto sourceObject = ObjectManager::get<VehicleObject>(sourceVehicleTypeId); // esi

        if (newObject->hasFlags(VehicleObjectFlags::cannotCoupleToSelf) && sourceObject->hasFlags(VehicleObjectFlags::cannotCoupleToSelf))
        {
            GameCommands::setErrorText(StringIds::incompatible_vehicle);
            return false;
        }

        if (newVehicleTypeId == sourceVehicleTypeId)
        {
            return true;
        }

        for (auto i = 0; i < newObject->numCompatibleVehicles; ++i)
        {
            if (newObject->compatibleVehicles[i] == sourceVehicleTypeId)
            {
                return true;
            }
        }

        if (sourceObject->numCompatibleVehicles != 0)
        {
            for (auto i = 0; i < sourceObject->numCompatibleVehicles; ++i)
            {
                if (sourceObject->compatibleVehicles[i] == newVehicleTypeId)
                {
                    return true;
                }
            }
        }

        if ((newObject->numCompatibleVehicles != 0) || (sourceObject->numCompatibleVehicles != 0))
        {
            GameCommands::setErrorText(StringIds::incompatible_vehicle);
            return false;
        }

        return true;
    }

    // 0x004B97B7
    uint32_t VehicleHead::getVehicleTotalLength() const
    {
        auto totalLength = 0;
        Vehicle train(head);
        for (const auto& car : train.cars)
        {
            totalLength += car.body->getObject()->getLength();
        }
        return totalLength;
    }

    uint32_t VehicleHead::getCarCount() const
    {
        Vehicle train(head);
        return train.cars.size();
    }

    // 0x004B8FA2
    // esi : self
    // ax  : vehicleTypeId
    bool VehicleHead::isVehicleTypeCompatible(const uint16_t vehicleTypeId) // TODO: const
    {
        auto newObject = ObjectManager::get<VehicleObject>(vehicleTypeId);
        if (newObject->mode == TransportMode::air || newObject->mode == TransportMode::water)
        {
            Vehicle train(head);
            if (!train.cars.empty())
            {
                GameCommands::setErrorText(StringIds::incompatible_vehicle);
                return false;
            }
        }
        else
        {
            if (newObject->trackType != trackType)
            {
                GameCommands::setErrorText(StringIds::incompatible_vehicle);
                return false;
            }
        }

        if (newObject->mode != mode)
        {
            GameCommands::setErrorText(StringIds::incompatible_vehicle);
            return false;
        }

        if (newObject->type != vehicleType)
        {
            GameCommands::setErrorText(StringIds::incompatible_vehicle);
            return false;
        }

        {
            Vehicle train(head);
            for (const auto& car : train.cars)
            {
                // The objectId is the same for all vehicle components and car components of a car
                if (!canVehiclesCouple(vehicleTypeId, car.front->objectId))
                {
                    return false;
                }
            }
        }
        if (mode != TransportMode::road)
        {
            return true;
        }

        if (trackType != 0xFF)
        {
            return true;
        }

        auto curTotalLength = getVehicleTotalLength();
        auto additionalNewLength = ObjectManager::get<VehicleObject>(vehicleTypeId)->getLength();
        if (curTotalLength + additionalNewLength > kMaxRoadVehicleLength)
        {
            GameCommands::setErrorText(StringIds::vehicle_too_long);
            return false;
        }
        return true;
    }

    // 0x004B671C
    VehicleStatus VehicleHead::getStatus() const
    {
        VehicleStatus vehStatus = {};
        vehStatus.status2 = StringIds::null;
        if (tileX == -1)
        {
            vehStatus.status1 = StringIds::vehicle_status_no_position;
            return vehStatus;
        }
        Vehicle train(head);
        if (train.veh2->has73Flags(Flags73::isBrokenDown))
        {
            vehStatus.status1 = StringIds::vehicle_status_broken_down;
            return vehStatus;
        }

        switch (status)
        {
            case Status::unk_0:
                vehStatus.status1 = StringIds::vehicle_status_no_position;
                return vehStatus;
            case Status::stopped:
                vehStatus.status1 = StringIds::vehicle_status_stopped;
                return vehStatus;
            case Status::travelling:
            case Status::waitingAtSignal:
                return getStatusTravelling();
            case Status::approaching:
            {
                if (stationId == StationId::null)
                {
                    return getStatusTravelling();
                }
                vehStatus.status1 = StringIds::vehicle_status_approaching;
                auto station = StationManager::get(stationId);
                vehStatus.status1Args = station->name | (enumValue(station->town) << 16);
                getSecondStatus(vehStatus);
                return vehStatus;
            }
            case Status::unloading:
            {
                if (stationId == StationId::null)
                {
                    return getStatusTravelling();
                }
                vehStatus.status1 = StringIds::vehicle_status_unloading;
                auto station = StationManager::get(stationId);
                vehStatus.status1Args = station->name | (enumValue(station->town) << 16);
                return vehStatus;
            }
            case Status::loading:
            {
                if (stationId == StationId::null)
                {
                    return getStatusTravelling();
                }
                vehStatus.status1 = StringIds::vehicle_status_loading;
                auto station = StationManager::get(stationId);
                vehStatus.status1Args = station->name | (enumValue(station->town) << 16);
                return vehStatus;
            }
            case Status::brokenDown:
                vehStatus.status1 = StringIds::vehicle_status_broken_down;
                return vehStatus;
            case Status::crashed:
                vehStatus.status1 = StringIds::vehicle_status_crashed;
                return vehStatus;
            case Status::stuck:
                vehStatus.status1 = StringIds::vehicle_status_stuck;
                return vehStatus;
            case Status::landing:
            {
                if (stationId == StationId::null)
                {
                    return getStatusTravelling();
                }
                vehStatus.status1 = StringIds::vehicle_status_landing;
                auto station = StationManager::get(stationId);
                vehStatus.status1Args = station->name | (enumValue(station->town) << 16);
                getSecondStatus(vehStatus);
                return vehStatus;
            }
            case Status::taxiing1:
            case Status::taxiing2:
            {
                if (stationId == StationId::null)
                {
                    return getStatusTravelling();
                }
                vehStatus.status1 = StringIds::vehicle_status_taxiing;
                auto station = StationManager::get(stationId);
                vehStatus.status1Args = station->name | (enumValue(station->town) << 16);
                getSecondStatus(vehStatus);
                return vehStatus;
            }
            case Status::takingOff:
            {
                if (stationId == StationId::null)
                {
                    return getStatusTravelling();
                }
                vehStatus.status1 = StringIds::vehicle_status_taking_off;
                auto station = StationManager::get(stationId);
                vehStatus.status1Args = station->name | (enumValue(station->town) << 16);
                getSecondStatus(vehStatus);
                return vehStatus;
            }
        }
        return vehStatus;
    }

    // 0x004B6885
    VehicleStatus VehicleHead::getStatusTravelling() const
    {
        VehicleStatus vehStatus{};

        if (hasVehicleFlags(VehicleFlags::commandStop) || (hasVehicleFlags(VehicleFlags::manualControl) && manualPower <= -20))
        {
            vehStatus.status1 = StringIds::vehicle_status_stopping;
        }
        else if (sizeOfOrderTable == 1)
        {
            vehStatus.status1 = StringIds::vehicle_status_travelling;
        }
        else
        {
            bool stopFound = false;
            auto orders = getCurrentOrders();
            for (auto& order : orders)
            {
                auto* stopOrder = order.as<OrderStopAt>();
                if (stopOrder == nullptr)
                {
                    continue;
                }

                stopFound = true;
                auto* station = StationManager::get(stopOrder->getStation());
                vehStatus.status1 = StringIds::vehicle_status_heading_for;
                vehStatus.status1Args = (enumValue(station->town) << 16) | station->name;
                break;
            }
            if (!stopFound)
            {
                vehStatus.status1 = StringIds::vehicle_status_travelling;
            }
        }
        getSecondStatus(vehStatus);
        return vehStatus;
    }

    // 0x004B691C
    void VehicleHead::getSecondStatus(VehicleStatus& vehStatus) const
    {
        Vehicle train(head);
        if (status == Status::waitingAtSignal)
        {
            vehStatus.status2 = StringIds::vehicle_status_waiting_at_signal;
        }
        else
        {
            vehStatus.status2 = StringIds::vehicle_status_at_velocity;
            vehStatus.status2Args = toSpeed16(train.veh2->currentSpeed).getRaw();
        }
    }
    // 0x004A8882
    void VehicleHead::updateDrivingSounds()
    {
        Vehicle train(head);
        updateDrivingSound(train.veh2->getSoundPlayer());
        updateDrivingSound(train.tail->getSoundPlayer());
    }

    // 0x004A88A6
    void VehicleHead::updateDrivingSound(VehicleSoundPlayer* soundPlayer)
    {
        if (tileX == -1 || status == Status::crashed || status == Status::stuck || has38Flags(Flags38::isGhost) || soundPlayer->objectId == 0xFFFF)
        {
            updateDrivingSoundNone(soundPlayer);
            return;
        }

        auto vehicleObject = ObjectManager::get<VehicleObject>(soundPlayer->objectId);
        switch (vehicleObject->drivingSoundType)
        {
            case DrivingSoundType::none:
                updateDrivingSoundNone(soundPlayer);
                break;
            case DrivingSoundType::friction:
                updateDrivingSoundFriction(soundPlayer, &vehicleObject->sound.friction);
                break;
            case DrivingSoundType::simpleMotor:
                updateSimpleMotorSound(soundPlayer, &vehicleObject->sound.simpleMotor);
                break;
            case DrivingSoundType::gearboxMotor:
                updateGearboxMotorSound(soundPlayer, &vehicleObject->sound.gearboxMotor);
                break;
            default:
                break;
        }
    }

    // 0x004A8B7C
    void VehicleHead::updateDrivingSoundNone(VehicleSoundPlayer* soundPlayer)
    {
        soundPlayer->drivingSoundId = 0xFF;
    }

    // 0x004A88F7
    void VehicleHead::updateDrivingSoundFriction(VehicleSoundPlayer* soundPlayer, const VehicleObjectFrictionSound* snd)
    {
        Vehicle2* vehType2_2 = _vehicleUpdate_2;
        if (vehType2_2->currentSpeed < snd->minSpeed)
        {
            updateDrivingSoundNone(soundPlayer);
            return;
        }

        auto speedDiff = vehType2_2->currentSpeed - snd->minSpeed;
        soundPlayer->drivingSoundFrequency = (speedDiff.getRaw() >> snd->speedFreqFactor) + snd->baseFrequency;

        auto volume = (speedDiff.getRaw() >> snd->speedVolumeFactor) + snd->baseVolume;

        soundPlayer->drivingSoundVolume = std::min<uint8_t>(volume, snd->maxVolume);
        soundPlayer->drivingSoundId = snd->soundObjectId;
    }

    // 0x004A8937
    void VehicleHead::updateSimpleMotorSound(VehicleSoundPlayer* soundPlayer, const VehicleSimpleMotorSound* snd)
    {
        Vehicle train(head);
        if (soundPlayer->isVehicle2())
        {
            if (vehicleType != VehicleType::ship && vehicleType != VehicleType::aircraft)
            {
                // Can be a type 6 or bogie
                if (train.cars.empty())
                {
                    assert(false);
                }
                if (train.cars.firstCar.front->hasBreakdownFlags(BreakdownFlags::brokenDown))
                {
                    updateDrivingSoundNone(soundPlayer);
                    return;
                }
            }
        }

        Vehicle2* vehType2_2 = _vehicleUpdate_2;
        uint16_t targetFrequency = snd->idleFrequency;
        uint8_t targetVolume = snd->idleVolume;

        if (vehType2_2->motorState == MotorState::accelerating && (!(soundPlayer->isVehicle2()) || train.cars.firstCar.front->wheelSlipping == 0))
        {
            targetFrequency = snd->accelerationBaseFreq + (vehType2_2->currentSpeed.getRaw() >> snd->speedFreqFactor);
            targetVolume = snd->acclerationVolume;
        }
        else if (vehType2_2->motorState == MotorState::coasting && vehType2_2->currentSpeed >= 12.0_mph)
        {
            targetFrequency = snd->coastingFrequency;
            targetVolume = snd->coastingVolume;
        }

        if (soundPlayer->drivingSoundId == 0xFF)
        {
            // Half
            soundPlayer->drivingSoundVolume = snd->idleVolume >> 1;
            // Quarter
            soundPlayer->drivingSoundFrequency = snd->idleFrequency >> 2;
            soundPlayer->drivingSoundId = snd->soundObjectId;
            return;
        }

        if (soundPlayer->drivingSoundFrequency != targetFrequency)
        {
            if (soundPlayer->drivingSoundFrequency > targetFrequency)
            {
                soundPlayer->drivingSoundFrequency = std::max<uint16_t>(targetFrequency, soundPlayer->drivingSoundFrequency - snd->freqDecreaseStep);
            }
            else
            {
                soundPlayer->drivingSoundFrequency = std::min<uint16_t>(targetFrequency, soundPlayer->drivingSoundFrequency + snd->freqIncreaseStep);
            }
        }

        if (soundPlayer->drivingSoundVolume != targetVolume)
        {
            if (soundPlayer->drivingSoundVolume > targetVolume)
            {
                soundPlayer->drivingSoundVolume = std::max<uint8_t>(targetVolume, soundPlayer->drivingSoundVolume - snd->volumeDecreaseStep);
            }
            else
            {
                soundPlayer->drivingSoundVolume = std::min<uint8_t>(targetVolume, soundPlayer->drivingSoundVolume + snd->volumeIncreaseStep);
            }
        }

        soundPlayer->drivingSoundId = snd->soundObjectId;
    }

    // 0x004A8A39
    void VehicleHead::updateGearboxMotorSound(VehicleSoundPlayer* soundPlayer, const VehicleGearboxMotorSound* snd)
    {
        Vehicle train(head);
        if (soundPlayer->isVehicle2())
        {
            if (vehicleType != VehicleType::ship && vehicleType != VehicleType::aircraft)
            {
                // Can be a type 6 or bogie
                if (train.cars.empty())
                {
                    assert(false);
                }
                if (train.cars.firstCar.front->hasBreakdownFlags(BreakdownFlags::brokenDown))
                {
                    updateDrivingSoundNone(soundPlayer);
                    return;
                }
            }
        }

        Vehicle2* vehType2_2 = _vehicleUpdate_2;
        uint16_t targetFrequency = 0;
        uint8_t targetVolume = 0;
        bool transmissionInGear = vehType2_2->motorState == MotorState::accelerating;

        if (vehType2_2->motorState == MotorState::coasting || vehType2_2->motorState == MotorState::braking)
        {
            targetVolume = snd->coastingVolume;
            if (vehType2_2->currentSpeed < 12.0_mph)
            {
                targetFrequency = snd->idleFrequency;
            }
            else
            {
                transmissionInGear = true;
            }
        }
        else if (vehType2_2->motorState == MotorState::accelerating)
        {
            targetVolume = snd->acceleratingVolume;
        }
        else
        {
            targetFrequency = snd->idleFrequency;
            targetVolume = snd->idleVolume;
        }

        if (transmissionInGear == true)
        {
            if (!(soundPlayer->isVehicle2()) || train.cars.firstCar.front->wheelSlipping == 0)
            {
                auto speed = std::max(vehType2_2->currentSpeed, 7.0_mph);

                auto frequency = snd->firstGearFrequency;

                if (speed >= snd->firstGearSpeed)
                {
                    frequency -= snd->secondGearFreqOffset;
                    if (speed >= snd->secondGearSpeed)
                    {
                        frequency -= snd->thirdGearFreqOffset;
                        if (speed >= snd->thirdGearSpeed)
                        {
                            frequency -= snd->fourthGearFreqOffset;
                        }
                    }
                }
                targetFrequency = (speed.getRaw() >> snd->speedFreqFactor) + frequency;
            }
            else
            {
                targetFrequency = snd->idleFrequency;
                targetVolume = snd->idleVolume;
            }
        }

        if (soundPlayer->drivingSoundId == 0xFF)
        {
            // Half
            soundPlayer->drivingSoundVolume = snd->idleVolume >> 1;
            // Quarter
            soundPlayer->drivingSoundFrequency = snd->idleFrequency >> 2;
            soundPlayer->drivingSoundId = snd->soundObjectId;
            return;
        }

        if (soundPlayer->drivingSoundFrequency != targetFrequency)
        {
            if (soundPlayer->drivingSoundFrequency > targetFrequency)
            {
                targetVolume = snd->coastingVolume;
                soundPlayer->drivingSoundFrequency = std::max<uint16_t>(targetFrequency, soundPlayer->drivingSoundFrequency - snd->freqDecreaseStep);
            }
            else
            {
                soundPlayer->drivingSoundFrequency = std::min<uint16_t>(targetFrequency, soundPlayer->drivingSoundFrequency + snd->freqIncreaseStep);
            }
        }

        if (soundPlayer->drivingSoundVolume != targetVolume)
        {
            if (soundPlayer->drivingSoundVolume > targetVolume)
            {
                soundPlayer->drivingSoundVolume = std::max<uint8_t>(targetVolume, soundPlayer->drivingSoundVolume - snd->volumeDecreaseStep);
            }
            else
            {
                soundPlayer->drivingSoundVolume = std::min<uint8_t>(targetVolume, soundPlayer->drivingSoundVolume + snd->volumeIncreaseStep);
            }
        }

        soundPlayer->drivingSoundId = snd->soundObjectId;
    }

    // Returns veh1, veh2 position
    static std::pair<Pos2, Pos2> calculateNextPosition(const uint8_t yaw, const World::Pos2& curPos, const Vehicle1* veh1, const Speed32 speed)
    {
        auto dist = Math::Trigonometry::computeXYVector(speed.getRaw() >> 5, yaw);

        auto bigCoordX = veh1->var_4E + (curPos.x << 16) + dist.x;
        auto bigCoordY = veh1->var_50 + (curPos.y << 16) + dist.y;

        Pos2 veh1Pos = { static_cast<int16_t>(bigCoordX & 0xFFFF), static_cast<int16_t>(bigCoordY & 0xFFFF) };
        Pos2 veh2Pos = { static_cast<int16_t>(bigCoordX >> 16), static_cast<int16_t>(bigCoordY >> 16) };
        return std::make_pair(veh1Pos, veh2Pos);
    }

    // 0x004A8C11
    bool VehicleHead::updateLand()
    {
        Vehicle2* vehType2 = _vehicleUpdate_2;

        // If don't have any running issue and is approaching
        if ((!vehType2->has73Flags(Flags73::isBrokenDown) || vehType2->has73Flags(Flags73::isStillPowered)) && status == Status::approaching)
        {
            if (mode == TransportMode::road)
            {
                uint8_t bl = sub_4AA36A();
                if (bl == 1)
                {
                    return sub_4A8DB7();
                }
                else if (bl == 2)
                {
                    return sub_4A8F22();
                }
            }

            if (hasVehicleFlags(VehicleFlags::commandStop))
            {
                return sub_4A8CB6();
            }
            else if (hasVehicleFlags(VehicleFlags::manualControl))
            {
                if (manualPower <= -20)
                {
                    return sub_4A8C81();
                }
            }

            return landTryBeginUnloading();
        }

        if (status == Status::unloading)
        {
            updateUnloadCargo();

            return true;
        }
        else if (status == Status::loading)
        {
            return landLoadingUpdate();
        }
        else if (status == Status::crashed)
        {
            landCrashedUpdate();

            return false;
        }
        else if (status == Status::stuck)
        {
            return false;
        }
        else
        {
            status = Status::travelling;

            if (!vehType2->has73Flags(Flags73::isBrokenDown) || vehType2->has73Flags(Flags73::isStillPowered))
            {
                if (!hasVehicleFlags(VehicleFlags::manualControl) || manualPower > -20)
                {
                    if (!hasVehicleFlags(VehicleFlags::commandStop))
                    {
                        return landNormalMovementUpdate();
                    }
                    else
                    {
                        return sub_4A8CB6();
                    }
                }
                else
                {
                    return sub_4A8C81();
                }
            }
            else
            {
                return sub_4A8CB6();
            }
        }
    }

    // 0x004AA36A
    // 0: None of the below
    // 1: reached first timeout at signal
    // 2: give up and reverse at signal
    uint8_t VehicleHead::sub_4AA36A()
    {
        Vehicle train(head);
        if (train.veh2->routingHandle != train.veh1->routingHandle || train.veh2->subPosition != train.veh1->subPosition)
        {
            train.veh1->timeAtSignal = 0;
            return 0;
        }

        auto param1 = 160;
        auto turnaroundAtSignalTimeout = kBusSignalTimeout;

        if (trackType == 0xFF || ObjectManager::get<RoadObject>(trackType)->hasFlags(RoadObjectFlags::isRoad))
        {
            if (train.veh1->trackAndDirection.road.isBackToFront())
            {
                param1 = 128;
                turnaroundAtSignalTimeout = 544;
            }
        }
        else
        {
            // Tram
            turnaroundAtSignalTimeout = kTramSignalTimeout;
            if (train.veh1->trackAndDirection.road.isBackToFront())
            {
                param1 = 64;
                turnaroundAtSignalTimeout = 128;
            }
        }

        train.veh1->timeAtSignal++;
        if (train.veh1->timeAtSignal == param1)
        {
            return 1;
        }

        if (train.veh1->timeAtSignal == turnaroundAtSignalTimeout)
        {
            var_5C = 40;
            return 2;
        }

        return 0;
    }

    // 0x004A8DB7
    bool VehicleHead::sub_4A8DB7()
    {
        sub_4AD778();
        if (status == Status::approaching)
        {
            status = Status::travelling;
        }
        return true;
    }

    // 0x004A8F22
    bool VehicleHead::sub_4A8F22()
    {
        if (isOnExpectedRoadOrTrack())
        {
            auto temp = var_52;
            var_52 = 1;
            sub_4ADB47(false);
            var_52 = temp;
            return true;
        }
        else
        {
            Vehicle train(head);
            train.veh2->destroyTrain();
            return false;
        }
    }

    // 0x004A8CB6
    bool VehicleHead::sub_4A8CB6()
    {
        Vehicle1* vehType1 = _vehicleUpdate_1;

        if (position != vehType1->position)
        {
            sub_4AD93A();
            if (status == Status::approaching)
            {
                stationId = StationId::null;
                status = Status::travelling;
            }
        }

        Vehicle train(head);
        auto* vehType2 = train.veh2;
        if (vehType2->routingHandle != routingHandle || vehType2->subPosition != subPosition)
        {
            return true;
        }

        status = Status::stopped;
        vehType2 = _vehicleUpdate_2;

        if (vehType2->has73Flags(Flags73::isBrokenDown))
        {
            stationId = StationId::null;
            status = Status::brokenDown;
        }
        return true;
    }

    // 0x004A8C81
    bool VehicleHead::sub_4A8C81()
    {
        Vehicle2* vehType2 = _vehicleUpdate_2;
        if (vehType2->currentSpeed > 1.0_mph)
        {
            return landNormalMovementUpdate();
        }

        auto foundStationId = manualFindTrainStationAtLocation();
        if (foundStationId == StationId::null)
        {
            return sub_4A8CB6();
        }
        stationId = foundStationId;
        setStationVisitedTypes();
        checkIfAtOrderStation();
        updateLastJourneyAverageSpeed();
        beginUnloading();

        return sub_4A8CB6();
    }

    // 0x004A8FAC
    // Checks if at the desired station and then begins unloading if at it
    bool VehicleHead::landTryBeginUnloading()
    {
        Vehicle train(head);
        if (routingHandle != train.veh2->routingHandle || train.veh2->subPosition != subPosition)
        {
            return true;
        }

        // Manual control is going too fast at this point to stop at the station
        if (hasVehicleFlags(VehicleFlags::manualControl))
        {
            return true;
        }

        setStationVisitedTypes();
        checkIfAtOrderStation();
        updateLastJourneyAverageSpeed();
        beginUnloading();

        return true;
    }

    // 0x004A9011
    bool VehicleHead::landLoadingUpdate()
    {
        if (updateLoadCargo())
        {
            return true;
        }

        beginNewJourney();
        status = Status::stopped;
        advanceToNextRoutableOrder();

        if (hasVehicleFlags(VehicleFlags::manualControl))
        {
            return true;
        }

        if (sub_4ACCDC())
        {
            return sub_4A8F22();
        }

        return true;
    }

    // 0x004A8D48
    bool VehicleHead::landNormalMovementUpdate()
    {
        advanceToNextRoutableOrder();
        auto [al, flags, nextStation] = sub_4ACEE7(0xD4CB00, _vehicleUpdate_var_113612C);

        if (mode == TransportMode::road)
        {
            return roadNormalMovementUpdate(al, nextStation);
        }
        else
        {
            return trainNormalMovementUpdate(al, flags, nextStation);
        }
    }

    // 0x004A8D8F
    bool VehicleHead::roadNormalMovementUpdate(uint8_t al, StationId nextStation)
    {
        uint8_t bl = sub_4AA36A();
        if (bl == 1)
        {
            return sub_4A8DB7();
        }
        else if (bl == 2)
        {
            return sub_4A8F22();
        }
        else if (al == 4)
        {
            status = Status::approaching;
            stationId = nextStation;
            return true;
        }
        else if (al == 2)
        {
            Vehicle train(head);
            if (routingHandle != train.veh2->routingHandle || train.veh2->subPosition != subPosition)
            {
                return true;
            }
            return sub_4A8F22();
        }
        else
        {
            return true;
        }
    }

    // 0x004A8D63
    bool VehicleHead::trainNormalMovementUpdate(uint8_t al, uint8_t flags, StationId nextStation)
    {
        Vehicle train(head);
        if (al == 4)
        {
            status = Status::approaching;
            stationId = nextStation;
            return true;
        }
        else if (al == 3)
        {
            if (train.veh2->routingHandle != routingHandle || train.veh2->subPosition != subPosition)
            {
                return true;
            }

            status = Status::waitingAtSignal;

            if (!Config::get().trainsReverseAtSignals)
            {
                return true;
            }

            auto* vehType1 = train.veh1;
            vehType1->timeAtSignal++;

            if (hasVehicleFlags(VehicleFlags::manualControl))
            {
                var_5C = 2;
                vehType1->var_48 |= Flags48::passSignal;
                return true;
            }

            // if one-way or two-way signal??
            if (flags & (1 << 1))
            {
                if (vehType1->timeAtSignal >= kTrainOneWaySignalTimeout)
                {
                    if (flags & (1 << 7))
                    {
                        return landReverseFromSignal();
                    }

                    if (sub_4AC1C2())
                    {
                        var_5C = 2;
                        vehType1->var_48 |= Flags48::passSignal;
                        return true;
                    }
                    return landReverseFromSignal();
                }

                // Keep waiting at the signal
                return true;
            }
            else
            {
                if (!(vehType1->timeAtSignal & 0x3F))
                {
                    if (!(flags & (1 << 7)))
                    {
                        if (sub_4AC1C2())
                        {
                            var_5C = 2;
                            vehType1->var_48 |= Flags48::passSignal;
                            return true;
                        }
                    }

                    if (opposingTrainAtSignal())
                    {
                        return landReverseFromSignal();
                    }
                }

                if (vehType1->timeAtSignal >= kTrainTwoWaySignalTimeout)
                {
                    return landReverseFromSignal();
                }

                // Keep waiting at the signal
                return true;
            }
        }
        else
        {
            train.veh1->timeAtSignal = 0;
            if (al == 2)
            {
                if (hasVehicleFlags(VehicleFlags::manualControl))
                {
                    auto* vehType2 = train.veh2;
                    if (vehType2->routingHandle != routingHandle || vehType2->subPosition != subPosition)
                    {
                        return landReverseFromSignal();
                    }

                    // Crash
                    vehType2->destroyTrain();
                    return false;
                }

                return landReverseFromSignal();
            }
            else
            {
                return true;
            }
        }
    }

    // 0x004A8ED9
    bool VehicleHead::landReverseFromSignal()
    {
        Vehicle train(head);
        train.veh1->timeAtSignal = 0;

        if (routingHandle != train.veh2->routingHandle || train.veh2->subPosition != subPosition)
        {
            return true;
        }
        return sub_4A8F22();
    }

    // 0x004A9051
    bool VehicleHead::updateAir()
    {
        Vehicle2* vehType2 = _vehicleUpdate_2;

        if (vehType2->currentSpeed >= 20.0_mph)
        {
            _vehicleUpdate_var_1136130 = 0x4000;
        }
        else
        {
            _vehicleUpdate_var_1136130 = 0x2000;
        }

        Vehicle train(head);
        train.cars.firstCar.body->sub_4AAB0B();

        if (status == Status::stopped)
        {
            if (!hasVehicleFlags(VehicleFlags::commandStop))
            {
                setStationVisitedTypes();
                checkIfAtOrderStation();
                beginUnloading();
            }
            return true;
        }
        else if (status == Status::unloading)
        {
            updateUnloadCargo();
            return true;
        }
        else if (status == Status::loading)
        {
            return airplaneLoadingUpdate();
        }
        status = Status::travelling;
        auto [newStatus, targetSpeed] = airplaneGetNewStatus();

        status = newStatus;
        Vehicle1* vehType1 = _vehicleUpdate_1;
        vehType1->targetSpeed = targetSpeed;

        advanceToNextRoutableOrder();
        Speed32 type1speed = vehType1->targetSpeed;
        auto type2speed = vehType2->currentSpeed;

        if (type2speed == type1speed)
        {
            vehType2->motorState = MotorState::airplaneAtTaxiSpeed;

            if (type2speed != 20.0_mph)
            {
                vehType2->motorState = MotorState::coasting;
            }
        }
        else if (type2speed > type1speed)
        {
            vehType2->motorState = MotorState::coasting;
            auto decelerationAmount = 2.0_mph;
            if (type2speed >= 130.0_mph)
            {
                decelerationAmount = 5.0_mph;
                if (type2speed >= 400.0_mph)
                {
                    decelerationAmount = 11.0_mph;
                    if (type2speed >= 600.0_mph)
                    {
                        decelerationAmount = 25.0_mph;
                    }
                }
            }

            if (type1speed == 20.0_mph)
            {
                vehType2->motorState = MotorState::braking;
            }

            type2speed = std::max<Speed32>(0.0_mph, type2speed - decelerationAmount);
            type2speed = std::max<Speed32>(type1speed, type2speed);
            vehType2->currentSpeed = type2speed;
        }
        else
        {
            vehType2->motorState = MotorState::accelerating;
            type2speed += 2.0_mph;
            type2speed = std::min<Speed32>(type2speed, type1speed);
            vehType2->currentSpeed = type2speed;
        }

        auto [manhattanDistance, targetZ, targetYaw] = sub_427122();

        _vehicleUpdate_manhattanDistanceToStation = manhattanDistance;
        _vehicleUpdate_targetZ = targetZ;

        // Helicopter
        if ((_vehicleUpdate_helicopterAirportMovement & (AirportMovementNodeFlags::heliTakeoffEnd)) != AirportMovementNodeFlags::none)
        {
            _vehicleUpdate_helicopterTargetYaw = targetYaw;
            targetYaw = spriteYaw;
            vehType2->motorState = MotorState::accelerating;
            if (targetZ < position.z)
            {
                vehType2->motorState = MotorState::coasting;
            }
        }

        if (targetYaw != spriteYaw)
        {
            if (((targetYaw - spriteYaw) & 0x3F) > 0x20)
            {
                spriteYaw--;
            }
            else
            {
                spriteYaw++;
            }
            spriteYaw &= 0x3F;
        }

        Pitch targetPitch = Pitch::flat;

        auto vehObject = ObjectManager::get<VehicleObject>(train.cars.firstCar.front->objectId);

        if (vehType2->currentSpeed < 50.0_mph && vehObject->hasFlags(VehicleObjectFlags::aircraftIsTaildragger))
        {
            targetPitch = Pitch::up12deg;
        }

        if (targetZ > position.z)
        {
            if (vehType2->currentSpeed <= 350.0_mph)
            {
                targetPitch = Pitch::up12deg;
            }
        }

        if (targetZ < position.z)
        {
            if (vehType2->currentSpeed <= 180.0_mph && vehObject->hasFlags(VehicleObjectFlags::aircraftFlaresLanding))
            {
                targetPitch = Pitch::up12deg;
            }
        }

        if (targetPitch != spritePitch)
        {
            if (targetPitch < spritePitch)
            {
                spritePitch = Pitch(static_cast<uint8_t>(spritePitch) - 1);
            }
            else
            {
                spritePitch = Pitch(static_cast<uint8_t>(spritePitch) + 1);
            }
        }

        // Helicopter
        if ((_vehicleUpdate_helicopterAirportMovement & AirportMovementNodeFlags::heliTakeoffEnd) != AirportMovementNodeFlags::none)
        {
            vehType2->currentSpeed = 8.0_mph;
            if (targetZ != position.z)
            {
                return airplaneApproachTarget(targetZ);
            }
        }
        else
        {
            uint32_t targetTolerance = 480;
            if (airportMovementEdge != kAirportMovementNodeNull)
            {
                targetTolerance = 5;
                if (vehType2->currentSpeed >= 70.0_mph)
                {
                    targetTolerance = 30;
                }
            }

            if (manhattanDistance > targetTolerance)
            {
                return airplaneApproachTarget(targetZ);
            }
        }

        if (stationId != StationId::null && airportMovementEdge != kAirportMovementNodeNull)
        {
            auto flags = airportGetMovementEdgeTarget(stationId, airportMovementEdge).first;

            if ((flags & AirportMovementNodeFlags::touchdown) != AirportMovementNodeFlags::none)
            {
                produceTouchdownAirportSound();
            }
            if ((flags & AirportMovementNodeFlags::taxiing) != AirportMovementNodeFlags::none)
            {
                updateLastJourneyAverageSpeed();
            }

            if ((flags & AirportMovementNodeFlags::terminal) != AirportMovementNodeFlags::none)
            {
                return sub_4A95CB();
            }
        }

        auto movementEdge = kAirportMovementNodeNull;
        if (stationId != StationId::null)
        {
            movementEdge = airportMovementEdge;
        }

        auto newMovementEdge = airportGetNextMovementEdge(movementEdge);

        if (newMovementEdge != static_cast<uint8_t>(-2))
        {
            return sub_4A9348(newMovementEdge, targetZ);
        }

        if (vehType2->currentSpeed > 30.0_mph)
        {
            return airplaneApproachTarget(targetZ);
        }
        else
        {
            vehType2->currentSpeed = 0.0_mph;
            vehType2->motorState = MotorState::stopped;
            return true;
        }
    }

    // 0x004273DF
    std::pair<Status, Speed16> VehicleHead::airplaneGetNewStatus()
    {
        Vehicle train(head);
        if (stationId == StationId::null || airportMovementEdge == kAirportMovementNodeNull)
        {
            auto veh2 = train.veh2;
            auto targetSpeed = veh2->maxSpeed;
            if (veh2->has73Flags(Flags73::isBrokenDown))
            {
                targetSpeed = veh2->rackRailMaxSpeed;
            }

            return std::make_pair(Status::travelling, targetSpeed);
        }

        auto station = StationManager::get(stationId);

        Pos3 loc = station->airportStartPos;

        auto tile = World::TileManager::get(loc);
        for (auto& el : tile)
        {
            auto* elStation = el.as<World::StationElement>();
            if (elStation == nullptr)
            {
                continue;
            }

            if (elStation->baseZ() != loc.z / 4)
            {
                continue;
            }

            auto airportObject = ObjectManager::get<AirportObject>(elStation->objectId());

            uint8_t al = airportObject->movementEdges[airportMovementEdge].var_03;
            uint8_t cl = airportObject->movementEdges[airportMovementEdge].var_00;

            auto veh2 = train.veh2;
            if (al != 0)
            {
                if (cl == 1 || al != 2)
                {
                    if (al == 1)
                    {
                        return std::make_pair(Status::landing, veh2->rackRailMaxSpeed);
                    }
                    else if (al == 3)
                    {
                        return std::make_pair(Status::landing, 0_mph);
                    }
                    else if (al == 4)
                    {
                        return std::make_pair(Status::taxiing1, 20_mph);
                    }
                    else
                    {
                        return std::make_pair(Status::approaching, veh2->rackRailMaxSpeed);
                    }
                }
            }

            if (cl == 2)
            {
                auto targetSpeed = veh2->maxSpeed;
                if (veh2->has73Flags(Flags73::isBrokenDown))
                {
                    targetSpeed = veh2->rackRailMaxSpeed;
                }
                return std::make_pair(Status::takingOff, targetSpeed);
            }
            else if (cl == 3)
            {
                return std::make_pair(Status::takingOff, 0_mph);
            }
            else
            {
                return std::make_pair(Status::taxiing2, 20_mph);
            }
        }

        // Tile not found. Todo: fail gracefully
        assert(false);
        return std::make_pair(Status::travelling, train.veh2->maxSpeed);
    }

    // 0x004A95CB
    bool VehicleHead::sub_4A95CB()
    {
        if (hasVehicleFlags(VehicleFlags::commandStop))
        {
            status = Status::stopped;
            Vehicle2* vehType2 = _vehicleUpdate_2;
            vehType2->currentSpeed = 0.0_mph;
        }
        else
        {
            setStationVisitedTypes();
            checkIfAtOrderStation();
            beginUnloading();
        }
        return true;
    }

    // 0x004A95F5
    bool VehicleHead::airplaneLoadingUpdate()
    {
        Vehicle2* vehType2 = _vehicleUpdate_2;
        vehType2->currentSpeed = 0.0_mph;
        vehType2->motorState = MotorState::stopped;
        if (updateLoadCargo())
        {
            return true;
        }

        advanceToNextRoutableOrder();
        status = Status::travelling;
        status = airplaneGetNewStatus().first;

        auto movementEdge = kAirportMovementNodeNull;
        if (stationId != StationId::null)
        {
            movementEdge = airportMovementEdge;
        }

        auto newMovementEdge = airportGetNextMovementEdge(movementEdge);

        if (newMovementEdge != static_cast<uint8_t>(-2))
        {
            // Strangely the original would enter this function with an
            // uninitialised targetZ. We will pass a valid z.
            return sub_4A9348(newMovementEdge, position.z);
        }

        status = Status::loading;
        return true;
    }

    // 0x004A94A9
    bool VehicleHead::airplaneApproachTarget(uint16_t targetZ)
    {
        auto yaw = spriteYaw;
        // Helicopter
        if ((_vehicleUpdate_helicopterAirportMovement & AirportMovementNodeFlags::heliTakeoffEnd) != AirportMovementNodeFlags::none)
        {
            yaw = _vehicleUpdate_helicopterTargetYaw;
        }

        Vehicle1* vehType1 = _vehicleUpdate_1;
        Vehicle2* vehType2 = _vehicleUpdate_2;

        auto [veh1Loc, veh2Loc] = calculateNextPosition(
            yaw, position, vehType1, vehType2->currentSpeed);

        Pos3 newLoc(veh2Loc.x, veh2Loc.y, targetZ);
        vehType1->var_4E = veh1Loc.x;
        vehType1->var_50 = veh1Loc.y;
        if (targetZ != position.z)
        {
            // Final section of landing / helicopter
            if (_vehicleUpdate_manhattanDistanceToStation <= 28)
            {
                int16_t zShift = 1;
                if (vehType2->currentSpeed >= 50.0_mph)
                {
                    zShift++;
                    if (vehType2->currentSpeed >= 100.0_mph)
                    {
                        zShift++;
                    }
                }

                if (targetZ < position.z)
                {
                    newLoc.z = std::max<int16_t>(targetZ, position.z - zShift);
                }
                else if (targetZ > position.z)
                {
                    newLoc.z = std::min<int16_t>(targetZ, position.z + zShift);
                }
            }
            else
            {
                int32_t zDiff = targetZ - position.z;
                // We want a SAR instruction so use >>5
                int32_t param1 = (zDiff * toSpeed16(vehType2->currentSpeed).getRaw()) >> 5;
                int32_t param2 = _vehicleUpdate_manhattanDistanceToStation - 18;

                auto modulo = param1 % param2;
                if (modulo < 0)
                {
                    newLoc.z = position.z + param1 / param2 - 1;
                }
                else
                {
                    newLoc.z = position.z + param1 / param2 + 1;
                }
            }
        }
        movePlaneTo(newLoc, spriteYaw, spritePitch);
        return true;
    }

    bool VehicleHead::sub_4A9348(uint8_t newMovementEdge, uint16_t targetZ)
    {
        if (stationId != StationId::null && airportMovementEdge != kAirportMovementNodeNull)
        {
            StationManager::get(stationId)->airportMovementOccupiedEdges &= ~(1 << airportMovementEdge);
        }

        if (newMovementEdge == kAirportMovementNodeNull)
        {
            beginNewJourney();

            if (sizeOfOrderTable == 1)
            {
                // 0x4a94a5
                airportMovementEdge = kAirportMovementNodeNull;
                return airplaneApproachTarget(targetZ);
            }

            auto orders = getCurrentOrders();
            auto* order = orders.begin()->as<OrderStopAt>();
            if (order == nullptr)
            {
                airportMovementEdge = kAirportMovementNodeNull;
                return airplaneApproachTarget(targetZ);
            }

            StationId orderStationId = order->getStation();

            auto station = StationManager::get(orderStationId);

            if (station == nullptr || (station->flags & StationFlags::flag_6) == StationFlags::none)
            {
                airportMovementEdge = kAirportMovementNodeNull;
                return airplaneApproachTarget(targetZ);
            }

            if (!CompanyManager::isPlayerCompany(owner))
            {
                stationId = orderStationId;
                airportMovementEdge = kAirportMovementNodeNull;
                return airplaneApproachTarget(targetZ);
            }

            Pos3 loc = station->airportStartPos;

            auto tile = World::TileManager::get(loc);
            for (auto& el : tile)
            {
                auto* elStation = el.as<World::StationElement>();
                if (elStation == nullptr)
                {
                    continue;
                }

                if (elStation->baseZ() != loc.z / 4)
                {
                    continue;
                }

                auto airportObject = ObjectManager::get<AirportObject>(elStation->objectId());
                Vehicle train(head);
                const auto airportType = train.cars.firstCar.front->getCompatibleAirportType();

                if ((airportObject->flags & airportType) != AirportObjectFlags::none)
                {
                    stationId = orderStationId;
                    airportMovementEdge = kAirportMovementNodeNull;
                    return airplaneApproachTarget(targetZ);
                }

                if (owner == CompanyManager::getControllingId())
                {
                    MessageManager::post(
                        MessageType::unableToLandAtAirport,
                        owner,
                        enumValue(id),
                        enumValue(orderStationId));
                }

                airportMovementEdge = kAirportMovementNodeNull;
                return airplaneApproachTarget(targetZ);
            }

            // Todo: fail gracefully on tile not found
            assert(false);

            return true;
            // 0x004A938A
        }
        else
        {
            airportMovementEdge = newMovementEdge;
            if (stationId != StationId::null)
            {
                auto station = StationManager::get(stationId);
                station->airportMovementOccupiedEdges |= (1 << airportMovementEdge);
            }
            return airplaneApproachTarget(targetZ);
        }
    }

    // 0x004A9649
    bool VehicleHead::updateWater()
    {
        Vehicle2* vehType2 = _vehicleUpdate_2;
        if (vehType2->currentSpeed >= 5.0_mph)
        {
            _vehicleUpdate_var_1136130 = 0x4000;
        }
        else
        {
            _vehicleUpdate_var_1136130 = 0x2000;
        }

        Vehicle train(head);
        train.cars.firstCar.body->sub_4AAB0B();

        if (status == Status::stopped)
        {
            if (hasVehicleFlags(VehicleFlags::commandStop))
            {
                return true;
            }

            if (stationId != StationId::null)
            {
                vehType2->currentSpeed = 0.0_mph;
                setStationVisitedTypes();
                checkIfAtOrderStation();
                updateLastJourneyAverageSpeed();
                beginUnloading();
                return true;
            }
        }

        if (hasVehicleFlags(VehicleFlags::commandStop))
        {
            if ((updateWaterMotion(WaterMotionFlags::isStopping) & WaterMotionFlags::hasReachedADestination) == WaterMotionFlags::none)
            {
                return true;
            }

            status = Status::stopped;
            vehType2->currentSpeed = 0.0_mph;
            vehType2->motorState = MotorState::stopped;
            return true;
        }

        if (status == Status::unloading)
        {
            updateUnloadCargo();
            return true;
        }
        else if (status == Status::loading)
        {
            if (updateLoadCargo())
            {
                return true;
            }

            beginNewJourney();
            advanceToNextRoutableOrder();
            status = Status::travelling;
            status = sub_427BF2();
            updateWaterMotion(WaterMotionFlags::isLeavingDock);
            produceLeavingDockSound();
            return true;
        }
        else
        {
            status = Status::travelling;
            status = sub_427BF2();
            advanceToNextRoutableOrder();
            if ((updateWaterMotion(WaterMotionFlags::none) & WaterMotionFlags::hasReachedDock) == WaterMotionFlags::none)
            {
                return true;
            }

            if (hasVehicleFlags(VehicleFlags::commandStop))
            {
                status = Status::stopped;
                vehType2->currentSpeed = 0.0_mph;
                vehType2->motorState = MotorState::stopped;
                return true;
            }

            vehType2->currentSpeed = 0.0_mph;
            setStationVisitedTypes();
            checkIfAtOrderStation();
            updateLastJourneyAverageSpeed();
            beginUnloading();
            return true;
        }
    }

    /** 0x00427122
     * Seems to work out where to land or something like that.
     *  manhattanDistance = regs.ebp
     *  targetZ = regs.dx
     *  targetYaw = regs.bl
     *  airportFlags = _vehicleUpdate_var_525BB0
     */
    std::tuple<uint32_t, uint16_t, uint8_t> VehicleHead::sub_427122()
    {
        _vehicleUpdate_helicopterAirportMovement = AirportMovementNodeFlags::none;
        StationId targetStationId = StationId::null;
        std::optional<World::Pos3> targetPos{};
        if (stationId == StationId::null)
        {
            auto orders = getCurrentOrders();
            auto curOrder = orders.begin();
            auto stationOrder = curOrder->as<OrderStation>();
            if (curOrder->is<OrderRouteWaypoint>() || !(curOrder->hasFlags(OrderFlags::HasStation)) || stationOrder == nullptr)
            {
                targetStationId = stationId;
            }
            else
            {
                targetStationId = stationOrder->getStation();
            }
        }
        else
        {
            if (airportMovementEdge == kAirportMovementNodeNull)
            {
                targetStationId = stationId;
            }
            else
            {
                auto station = StationManager::get(stationId);
                if ((station->flags & StationFlags::flag_6) == StationFlags::none)
                {
                    targetStationId = stationId;
                }
                else
                {
                    auto [flags, pos] = airportGetMovementEdgeTarget(stationId, airportMovementEdge);
                    _vehicleUpdate_helicopterAirportMovement = flags;
                    targetPos = pos;
                }
            }
        }

        if (!targetPos)
        {
            if (targetStationId == StationId::null)
            {
                targetPos = Pos3{ 6143, 6143, 960 };
            }
            else
            {
                auto station = StationManager::get(targetStationId);
                targetPos = Pos3{ station->x, station->y, 960 };
                if ((station->flags & StationFlags::flag_6) != StationFlags::none)
                {
                    targetPos = Pos3{ station->airportStartPos.x, station->airportStartPos.y, 960 };
                }
            }
        }

        auto xDiff = targetPos->x - position.x;
        auto yDiff = targetPos->y - position.y;

        auto targetYaw = calculateYaw1FromVectorPlane(xDiff, yDiff);

        // Manhattan distance to target
        auto manhattanDistance = Math::Vector::manhattanDistance2D(World::Pos2{ position }, World::Pos2{ *targetPos });

        // Manhattan distance, targetZ, targetYaw
        return std::make_tuple(manhattanDistance, targetPos->z, targetYaw);
    }

    // 0x00427214 returns next movement edge or -2 if no valid edge or -1 for in flight
    uint8_t VehicleHead::airportGetNextMovementEdge(uint8_t curEdge)
    {
        auto station = StationManager::get(stationId);

        Pos3 loc = station->airportStartPos;

        auto tile = TileManager::get(loc);

        for (auto& el : tile)
        {
            auto* elStation = el.as<StationElement>();
            if (elStation == nullptr)
            {
                continue;
            }

            if (elStation->baseZ() != loc.z / 4)
            {
                continue;
            }

            auto airportObject = ObjectManager::get<AirportObject>(elStation->objectId());

            if (curEdge == kAirportMovementNodeNull)
            {
                for (uint8_t movementEdge = 0; movementEdge < airportObject->numMovementEdges; movementEdge++)
                {
                    const auto& transition = airportObject->movementEdges[movementEdge];
                    if (!airportObject->movementNodes[transition.curNode].hasFlags(AirportMovementNodeFlags::flag2))
                    {
                        continue;
                    }

                    if (station->airportMovementOccupiedEdges & transition.mustBeClearEdges)
                    {
                        continue;
                    }

                    if (transition.atLeastOneClearEdges == 0)
                    {
                        return movementEdge;
                    }

                    auto occupiedAreas = station->airportMovementOccupiedEdges & transition.atLeastOneClearEdges;
                    if (occupiedAreas == transition.atLeastOneClearEdges)
                    {
                        continue;
                    }

                    return movementEdge;
                }
                return kAirportMovementNoValidEdge;
            }
            else
            {
                uint8_t targetNode = airportObject->movementEdges[curEdge].nextNode;
                if (status == Status::takingOff && airportObject->movementNodes[targetNode].hasFlags(AirportMovementNodeFlags::takeoffEnd))
                {
                    return kAirportMovementNodeNull;
                }
                // 0x4272A5
                Vehicle train(head);
                auto vehObject = ObjectManager::get<VehicleObject>(train.cars.firstCar.front->objectId);
                if (vehObject->hasFlags(VehicleObjectFlags::aircraftIsHelicopter))
                {
                    for (uint8_t movementEdge = 0; movementEdge < airportObject->numMovementEdges; movementEdge++)
                    {
                        const auto& transition = airportObject->movementEdges[movementEdge];

                        if (transition.curNode != targetNode)
                        {
                            continue;
                        }

                        if (airportObject->movementNodes[transition.nextNode].hasFlags(AirportMovementNodeFlags::takeoffBegin))
                        {
                            continue;
                        }

                        if (station->airportMovementOccupiedEdges & transition.mustBeClearEdges)
                        {
                            continue;
                        }

                        if (transition.atLeastOneClearEdges == 0)
                        {
                            return movementEdge;
                        }

                        auto occupiedAreas = station->airportMovementOccupiedEdges & transition.atLeastOneClearEdges;
                        if (occupiedAreas == transition.atLeastOneClearEdges)
                        {
                            continue;
                        }
                        return movementEdge;
                    }

                    return kAirportMovementNoValidEdge;
                }
                else
                {
                    for (uint8_t movementEdge = 0; movementEdge < airportObject->numMovementEdges; movementEdge++)
                    {
                        const auto& transition = airportObject->movementEdges[movementEdge];
                        if (transition.curNode != targetNode)
                        {
                            continue;
                        }

                        if (airportObject->movementNodes[transition.nextNode].hasFlags(AirportMovementNodeFlags::heliTakeoffBegin))
                        {
                            continue;
                        }

                        if (station->airportMovementOccupiedEdges & transition.mustBeClearEdges)
                        {
                            continue;
                        }

                        if (transition.atLeastOneClearEdges == 0)
                        {
                            return movementEdge;
                        }

                        auto occupiedAreas = station->airportMovementOccupiedEdges & transition.atLeastOneClearEdges;
                        if (occupiedAreas == transition.atLeastOneClearEdges)
                        {
                            continue;
                        }

                        return movementEdge;
                    }
                    return kAirportMovementNoValidEdge;
                }
            }
        }

        // Tile not found. Todo: fail gracefully
        assert(false);
        return kAirportMovementNodeNull;
    }

    // 0x00426E26
    std::pair<AirportMovementNodeFlags, World::Pos3> VehicleHead::airportGetMovementEdgeTarget(StationId targetStation, uint8_t curEdge)
    {
        auto station = StationManager::get(targetStation);

        Pos3 stationLoc = station->airportStartPos;

        auto tile = TileManager::get(stationLoc);

        for (auto& el : tile)
        {
            auto* elStation = el.as<StationElement>();
            if (elStation == nullptr)
            {
                continue;
            }

            if (elStation->baseZ() != stationLoc.z / 4)
            {
                continue;
            }

            auto airportObject = ObjectManager::get<AirportObject>(elStation->objectId());

            auto destinationNode = airportObject->movementEdges[curEdge].nextNode;

            Pos2 loc2 = {
                static_cast<int16_t>(airportObject->movementNodes[destinationNode].x - 16),
                static_cast<int16_t>(airportObject->movementNodes[destinationNode].y - 16)
            };
            loc2 = Math::Vector::rotate(loc2, elStation->rotation());
            auto airportMovement = airportObject->movementNodes[destinationNode];

            loc2.x += 16 + stationLoc.x;
            loc2.y += 16 + stationLoc.y;

            Pos3 loc = { loc2.x, loc2.y, static_cast<int16_t>(airportObject->movementNodes[destinationNode].z + stationLoc.z) };

            if (!airportMovement.hasFlags(AirportMovementNodeFlags::taxiing))
            {
                loc.z = stationLoc.z + 255;
                if (!airportMovement.hasFlags(AirportMovementNodeFlags::inFlight))
                {
                    loc.z = 960;
                }
            }

            return std::make_pair(airportMovement.flags, loc);
        }

        // Tile not found. Todo: fail gracefully
        assert(false);
        // Flags, location
        return std::make_pair(AirportMovementNodeFlags::none, World::Pos3{ 0, 0, 0 });
    }

    // 0x004B980A
    void VehicleHead::tryCreateInitialMovementSound()
    {
        if (status != Status::travelling)
        {
            return;
        }

        if (_vehicleUpdate_initialStatus != Status::stopped && _vehicleUpdate_initialStatus != Status::waitingAtSignal)
        {
            return;
        }

        Vehicle train(head);
        const auto* vehObj = train.cars.firstCar.body->getObject();
        if (vehObj != nullptr && vehObj->numStartSounds != 0)
        {
            auto numSounds = vehObj->numStartSounds & NumStartSounds::kMask;
            if (vehObj->numStartSounds & NumStartSounds::kHasCrossingWhistle)
            {
                // remove the crossing whistle from available sounds to play
                numSounds = std::max(numSounds - 1, 1);
            }
            auto randSoundIndex = gPrng1().randNext(numSounds - 1);
            auto randSoundId = Audio::makeObjectSoundId(vehObj->startSounds[randSoundIndex]);
            Vehicle2* veh2 = _vehicleUpdate_2;
            auto tileHeight = TileManager::getHeight(veh2->position);
            auto volume = 0;
            if (veh2->position.z < tileHeight.landHeight)
            {
                volume = -1500;
            }
            Audio::playSound(randSoundId, veh2->position + World::Pos3{ 0, 0, 22 }, volume, 22050);
        }
    }

    // 0x004B996F
    void VehicleHead::setStationVisitedTypes()
    {
        auto station = StationManager::get(stationId);
        station->var_3B2 |= (1 << static_cast<uint8_t>(vehicleType));
    }

    // 0x004B9987
    void VehicleHead::checkIfAtOrderStation()
    {
        OrderRingView orders(orderTableOffset, currentOrder);
        auto curOrder = orders.begin();
        auto* orderStation = curOrder->as<OrderStation>();
        if (orderStation == nullptr)
        {
            return;
        }

        if (orderStation->getStation() != stationId)
        {
            return;
        }

        curOrder++;
        currentOrder = curOrder->getOffset() - orderTableOffset;
        Ui::WindowManager::sub_4B93A5(enumValue(id));
    }

    // 0x004BACAF
    void VehicleHead::updateLastJourneyAverageSpeed()
    {
        if (!hasBreakdownFlags(BreakdownFlags::journeyStarted))
        {
            return;
        }
        Vehicle train(*this);

        breakdownFlags &= ~BreakdownFlags::journeyStarted;
        const auto distanceTravelled = Math::Vector::distance2D(journeyStartPos, Pos2(train.veh2->position));

        const auto timeInTicks = ScenarioManager::getScenarioTicks() - journeyStartTicks;

        auto modeModifier = [](TransportMode mode) {
            switch (mode)
            {
                default:
                case TransportMode::rail:
                case TransportMode::road:
                    return 21;
                case TransportMode::air:
                    return 36;
                case TransportMode::water:
                    return 31;
            }
        }(mode);

        auto adjustedDistance = distanceTravelled * static_cast<int64_t>(modeModifier);
        auto adjustedTime = timeInTicks;
        // TODO: In the future (when we can diverge) we can just do a 64bit divide instead of
        // slowly bringing down the divisor and dividend by 2 until 32bit
        while (adjustedDistance > std::numeric_limits<uint32_t>::max())
        {
            adjustedDistance >>= 1;
            adjustedTime >>= 1;
        }
        if (adjustedTime == 0)
        {
            return;
        }
        const auto averageSpeed = std::min(train.veh2->maxSpeed, Speed16(adjustedDistance / adjustedTime));
        lastAverageSpeed = averageSpeed;

        Ui::WindowManager::invalidate(Ui::WindowType::vehicle, enumValue(head));

        const auto recordType = [](TransportMode mode) {
            switch (mode)
            {
                default:
                case TransportMode::rail:
                case TransportMode::road:
                    return 0;
                case TransportMode::air:
                    return 1;
                case TransportMode::water:
                    return 2;
            }
        }(mode);
        auto records = CompanyManager::getRecords();
        if (averageSpeed <= records.speed[recordType])
        {
            return;
        }

        records.speed[recordType] = averageSpeed;
        records.date[recordType] = getCurrentDay();
        records.company[recordType] = owner;
        CompanyManager::setRecords(records);

        MessageManager::post(MessageType::newSpeedRecord, owner, enumValue(head), enumValue(owner), recordType);
        companyEmotionEvent(owner, Emotion::happy);

        Ui::WindowManager::invalidate(Ui::WindowType::company, enumValue(owner));
    }

    // 0x004B99E1
    void VehicleHead::beginUnloading()
    {
        breakdownFlags &= ~BreakdownFlags::unk_0;
        status = Status::unloading;
        cargoTransferTimeout = 10;
        var_58 = 0;

        Vehicle train(head);
        train.cars.applyToComponents([](auto& component) { component.breakdownFlags |= BreakdownFlags::unk_0; });
    }

    // 0x00426CA4
    void VehicleHead::movePlaneTo(const World::Pos3& newLoc, const uint8_t newYaw, const Pitch newPitch)
    {
        Vehicle train(head);
        moveTo({ newLoc.x, newLoc.y, newLoc.z });
        tileX = 0;

        train.veh1->moveTo({ newLoc.x, newLoc.y, newLoc.z });
        train.veh1->tileX = 0;

        train.veh2->moveTo({ newLoc.x, newLoc.y, newLoc.z });
        train.veh2->tileX = 0;

        // The first bogie of the plane is the shadow of the plane
        auto* shadow = train.cars.firstCar.front;
        shadow->invalidateSprite();
        auto height = coord_t{ TileManager::getHeight(newLoc) };
        shadow->moveTo({ newLoc.x, newLoc.y, height });
        shadow->spriteYaw = newYaw;
        shadow->spritePitch = Pitch::flat;
        shadow->tileX = 0;
        shadow->invalidateSprite();

        auto* body = train.cars.firstCar.body;
        body->invalidateSprite();
        body->moveTo({ newLoc.x, newLoc.y, newLoc.z });
        body->spriteYaw = newYaw;
        body->spritePitch = newPitch;
        body->tileX = 0;
        body->invalidateSprite();
    }

    // 0x00427C05
    // Input flags:
    // bit 0  : commandedToStop
    // bit 1  : isLeavingDock
    // Output flags:
    // bit 16 : reachedDock
    // bit 17 : reachedADestination
    WaterMotionFlags VehicleHead::updateWaterMotion(WaterMotionFlags flags)
    {
        Vehicle2* veh2 = _vehicleUpdate_2;

        // updates the current boats position and sets flags about position
        auto tile = TileManager::get(veh2->position);

        SurfaceElement* surface = tile.surface();

        if (surface != nullptr)
        {
            auto waterHeight = surface->water();
            if (waterHeight != 0)
            {
                if (surface->isIndustrial())
                {
                    surface->setIsIndustrialFlag(false);
                    surface->setGrowthStage(0);
                }
                surface->setIndustry(IndustryId(0));
                surface->setType6Flag(true);
            }
        }

        auto targetSpeed = 5_mph;
        if (stationId == StationId::null)
        {
            if ((flags & WaterMotionFlags::isStopping) == WaterMotionFlags::none)
            {
                if (!veh2->has73Flags(Flags73::isBrokenDown))
                {
                    targetSpeed = veh2->maxSpeed;
                }
            }
        }

        if (targetSpeed == veh2->currentSpeed)
        {
            veh2->motorState = MotorState::coasting;
        }
        else if (targetSpeed < veh2->currentSpeed)
        {
            veh2->motorState = MotorState::coasting;
            auto decelerationRate = 1.0_mph;
            if (veh2->currentSpeed >= 50.0_mph)
            {
                decelerationRate = 3.0_mph;
            }
            veh2->motorState = MotorState::braking;
            auto newSpeed = std::max(veh2->currentSpeed - decelerationRate, 0.0_mph);
            veh2->currentSpeed = std::max<Speed32>(targetSpeed, newSpeed);
        }
        else
        {
            veh2->motorState = MotorState::accelerating;
            veh2->currentSpeed = std::min<Speed32>(targetSpeed, veh2->currentSpeed + 0.333333_mph);
        }

        auto manhattanDistance = Math::Vector::manhattanDistance2D(World::Pos2{ position }, World::Pos2{ veh2->position });
        auto targetTolerance = 3;
        if (veh2->currentSpeed >= 20.0_mph)
        {
            targetTolerance = 16;
            if (veh2->currentSpeed > 70.0_mph)
            {
                targetTolerance = 24;
            }
        }

        if (((flags & WaterMotionFlags::isLeavingDock) != WaterMotionFlags::none) || (manhattanDistance <= targetTolerance))
        {
            flags |= WaterMotionFlags::hasReachedADestination;
            if ((stationId != StationId::null)
                && ((flags & WaterMotionFlags::isLeavingDock) == WaterMotionFlags::none))
            {
                flags |= WaterMotionFlags::hasReachedDock;
            }
            if ((flags & WaterMotionFlags::isStopping) != WaterMotionFlags::none)
            {
                return flags;
            }

            OrderRingView orders(orderTableOffset, currentOrder);
            auto curOrder = orders.begin();
            auto waypoint = curOrder->as<OrderRouteWaypoint>();
            if (waypoint != nullptr)
            {
                auto point = waypoint->getWaypoint();
                if (point.x == (position.x & 0xFFE0) && point.y == (position.y & 0xFFE0))
                {
                    currentOrder = (++curOrder)->getOffset() - orderTableOffset;
                    Ui::WindowManager::sub_4B93A5(enumValue(id));
                }
            }

            if (((flags & WaterMotionFlags::isLeavingDock) == WaterMotionFlags::none) && (stationId != StationId::null))
            {
                return flags;
            }

            if (stationId != StationId::null)
            {
                auto targetTile = TileManager::get(World::Pos2{ tileX, tileY });
                StationElement* station = nullptr;
                for (auto& el : targetTile)
                {
                    station = el.as<StationElement>();
                    if (station == nullptr)
                    {
                        continue;
                    }
                    if (station->isGhost() || station->isAiAllocated())
                    {
                        continue;
                    }
                    if (station->baseZ() == tileBaseZ)
                    {
                        station->setFlag6(false);
                        stationId = StationId::null;
                        break;
                    }
                }
            }
            auto pathingResult = waterPathfind(*this);
            moveTo({ pathingResult.headTarget, 32 });

            if (pathingResult.stationId != StationId::null)
            {
                stationId = pathingResult.stationId;
                tileX = pathingResult.stationPos.x;
                tileY = pathingResult.stationPos.y;
                tileBaseZ = pathingResult.stationPos.z / 4;

                auto targetTile = TileManager::get(World::Pos2{ tileX, tileY });
                StationElement* station = nullptr;
                for (auto& el : targetTile)
                {
                    station = el.as<StationElement>();
                    if (station == nullptr)
                    {
                        continue;
                    }
                    if (station->isGhost() || station->isAiAllocated())
                    {
                        continue;
                    }
                    if (station->baseZ() == tileBaseZ)
                    {
                        station->setFlag6(true);
                        break;
                    }
                }
            }
        }

        auto targetYaw = calculateYaw4FromVector(position.x - veh2->position.x, position.y - veh2->position.y);
        if (targetYaw != veh2->spriteYaw)
        {
            if (((targetYaw - veh2->spriteYaw) & 0x3F) > 0x20)
            {
                veh2->spriteYaw--;
            }
            else
            {
                veh2->spriteYaw++;
            }
            veh2->spriteYaw &= 0x3F;
        }

        Vehicle1* veh1 = _vehicleUpdate_1;
        auto [newVeh1Pos, newVeh2Pos] = calculateNextPosition(veh2->spriteYaw, veh2->position, veh1, veh2->currentSpeed);

        veh1->var_4E = newVeh1Pos.x;
        veh1->var_50 = newVeh1Pos.y;

        Pos3 newLocation = { newVeh2Pos.x, newVeh2Pos.y, veh2->position.z };
        moveBoatTo(newLocation, veh2->spriteYaw, Pitch::flat);

        return flags;
    }

    // 0x00427B70
    void VehicleHead::moveBoatTo(const Pos3& newLoc, const uint8_t yaw, const Pitch pitch)
    {
        Vehicle train(head);
        train.veh1->moveTo({ newLoc.x, newLoc.y, newLoc.z });
        train.veh1->tileX = 0;
        train.veh2->moveTo({ newLoc.x, newLoc.y, newLoc.z });
        train.veh2->tileX = 0;
        train.cars.firstCar.body->invalidateSprite();
        train.cars.firstCar.body->moveTo({ newLoc.x, newLoc.y, newLoc.z });
        train.cars.firstCar.body->spriteYaw = yaw;
        train.cars.firstCar.body->spritePitch = pitch;
        train.cars.firstCar.body->tileX = 0;
        train.cars.firstCar.body->invalidateSprite();
    }

    uint8_t VehicleHead::getLoadingModifier(const VehicleBogie* bogie)
    {
        constexpr uint8_t kMinVehiclePastStationPenalty = 1;
        constexpr uint8_t kRailVehiclePastStationPenalty = 12;
        constexpr uint8_t kRoadVehiclePastStationPenalty = 2;

        switch (mode)
        {
            default:
            case TransportMode::air:
            case TransportMode::water:
                return kMinVehiclePastStationPenalty;
            case TransportMode::rail:
            {
                auto tile = World::TileManager::get(Pos2{ bogie->tileX, bogie->tileY });
                auto direction = bogie->trackAndDirection.track.cardinalDirection();
                auto trackId = bogie->trackAndDirection.track.id();
                auto loadingModifier = Config::get().disableVehicleLoadPenaltyCheat ? kMinVehiclePastStationPenalty : kRailVehiclePastStationPenalty;
                auto* elStation = tile.trainStation(trackId, direction, bogie->tileBaseZ);
                if (elStation != nullptr)
                {
                    if (elStation->isAiAllocated() || elStation->isGhost())
                    {
                        break;
                    }

                    if (elStation->stationId() != stationId)
                    {
                        break;
                    }

                    loadingModifier = kMinVehiclePastStationPenalty;
                }
                return loadingModifier;
            }
            case TransportMode::road:
            {
                auto tile = World::TileManager::get(Pos2{ bogie->tileX, bogie->tileY });
                auto direction = bogie->trackAndDirection.road.cardinalDirection();
                auto roadId = bogie->trackAndDirection.road.id();
                auto loadingModifier = Config::get().disableVehicleLoadPenaltyCheat ? kMinVehiclePastStationPenalty : kRoadVehiclePastStationPenalty;
                auto* elStation = tile.roadStation(roadId, direction, bogie->tileBaseZ);
                if (elStation != nullptr)
                {
                    if (elStation->isAiAllocated() || elStation->isGhost())
                    {
                        break;
                    }

                    if (elStation->stationId() != stationId)
                    {
                        break;
                    }

                    auto* roadStationObj = ObjectManager::get<RoadStationObject>(elStation->objectId());
                    if (!roadStationObj->hasFlags(RoadStationFlags::roadEnd))
                    {
                        breakdownFlags |= BreakdownFlags::unk_0;
                    }
                    loadingModifier = kMinVehiclePastStationPenalty;
                }
                return loadingModifier;
            }
        }
        return kMinVehiclePastStationPenalty;
    }

    // 0x004B9A88
    bool VehicleHead::updateUnloadCargoComponent(VehicleCargo& cargo, VehicleBogie* bogie)
    {
        if (cargo.qty == 0)
        {
            return false;
        }

        if (stationId == StationId::null)
        {
            return false;
        }

        auto* station = StationManager::get(stationId);
        auto& cargoStats = station->cargoStats[cargo.type];
        if (cargoStats.isAccepted())
        {
            station->deliverCargoToTown(cargo.type, cargo.qty);
            auto* sourceStation = StationManager::get(cargo.townFrom);
            auto stationLoc = World::Pos2{ station->x, station->y };
            auto sourceLoc = World::Pos2{ sourceStation->x, sourceStation->y };
            auto tilesDistance = Math::Vector::distance2D(stationLoc, sourceLoc) / 32;

            Ui::WindowManager::invalidate(Ui::WindowType::company, enumValue(owner));
            auto* company = CompanyManager::get(owner);
            company->cargoUnitsTotalDelivered += cargo.qty;

            auto cargoDist = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(cargo.qty) * tilesDistance, std::numeric_limits<uint32_t>::max()));
            company->cargoUnitsTotalDistance += cargoDist;

            auto cargoPayment = CompanyManager::calculateDeliveredCargoPayment(cargo.type, cargo.qty, tilesDistance, cargo.numDays);
            company->cargoDelivered[cargo.type] = Math::Bound::add(company->cargoDelivered[cargo.type], cargo.qty);

            updateLastIncomeStats(cargo.type, cargo.qty, tilesDistance, cargo.numDays, cargoPayment);

            var_58 += cargoPayment;
            station->var_3B1 = 0;
            station->flags |= StationFlags::flag_8;

            if (cargoStats.industryId != IndustryId::null)
            {
                auto* industry = IndustryManager::get(cargoStats.industryId);
                const auto* industryObj = industry->getObject();

                for (auto i = 0; i < 3; ++i)
                {
                    if (industryObj->requiredCargoType[i] != cargo.type)
                    {
                        continue;
                    }

                    industry->receivedCargoQuantityDailyTotal[i] = Math::Bound::add(industry->receivedCargoQuantityDailyTotal[i], cargo.qty);
                    industry->receivedCargoQuantityMonthlyTotal[i] = Math::Bound::add(industry->receivedCargoQuantityMonthlyTotal[i], cargo.qty);
                }

                if (!(industry->history_min_production[0] & (1ULL << cargo.type)))
                {
                    industry->history_min_production[0] |= 1ULL << cargo.type;
                    MessageManager::post(MessageType::workersCelebrate, owner, enumValue(id), enumValue(cargoStats.industryId), enumValue(cargoStats.industryId) | (cargo.type << 8));
                }

                auto* town = TownManager::get(industry->town);
                town->var_1A8 |= 1ULL << cargo.type;
                town = TownManager::get(station->town);
                town->var_1A8 |= 1ULL << cargo.type;
            }
            auto* town = TownManager::get(station->town);
            if (!(town->var_1A8 & (1ULL << cargo.type)))
            {
                town->var_1A8 |= 1ULL << cargo.type;
                MessageManager::post(MessageType::citizensCelebrate, owner, enumValue(id), enumValue(station->town), enumValue(station->town) | (cargo.type << 8));
            }

            if (cargoStats.isAccepted())
            {
                cargoStats.flags |= StationCargoStatsFlags::flag3;
            }

            company->var_4A0 |= 1ULL << cargo.type;
        }
        else
        {
            auto orders = getCurrentOrders();
            for (auto& order : orders)
            {
                if (!(order.hasFlags(OrderFlags::HasCargo)))
                {
                    return false;
                }
                auto* unloadOrder = order.as<OrderUnloadAll>();
                if (unloadOrder == nullptr)
                {
                    continue;
                }
                if (unloadOrder->getCargo() != cargo.type)
                {
                    continue;
                }

                break;
            }

            const bool stationHadPreviousCargo = cargoStats.quantity != 0;
            cargoStats.quantity = Math::Bound::add(cargoStats.quantity, cargo.qty);
            station->updateCargoDistribution();
            cargoStats.enrouteAge = Math::Bound::add(cargoStats.enrouteAge, cargo.numDays);

            // Change from vanilla to deal with the cargo transfer bug:
            // Calculate the weighted average of the present and the delivered cargo
            if (stationHadPreviousCargo)
            {
                // enrouteAge = enrouteAge * (1 - addedQuantity / summedQuantity)
                const auto multiplier = (1 << 16) - (cargo.qty << 16) / cargoStats.quantity;
                cargoStats.enrouteAge = (cargoStats.enrouteAge * multiplier) >> 16;
            }

            bool setOrigin = true;
            if (cargoStats.origin != StationId::null)
            {
                auto* cargoSourceStation = StationManager::get(cargoStats.origin);
                auto stationLoc = World::Pos2{ station->x, station->y };
                auto cargoSourceLoc = World::Pos2{ cargoSourceStation->x, cargoSourceStation->y };

                auto stationSourceDistance = Math::Vector::distance2D(stationLoc, cargoSourceLoc);

                auto* sourceStation = StationManager::get(cargo.townFrom);
                auto sourceLoc = World::Pos2{ sourceStation->x, sourceStation->y };
                auto cargoSourceDistance = Math::Vector::distance2D(stationLoc, sourceLoc);
                if (cargoSourceDistance > stationSourceDistance)
                {
                    setOrigin = false;
                }
            }
            if (setOrigin)
            {
                cargoStats.origin = cargo.townFrom;
            }
        }

        uint8_t loadingModifier = getLoadingModifier(bogie);

        auto* cargoObj = ObjectManager::get<CargoObject>(cargo.type);
        cargoTransferTimeout = static_cast<uint16_t>(std::min<uint32_t>((cargoObj->cargoTransferTime * cargo.qty * loadingModifier) / 256, std::numeric_limits<uint16_t>::max()));
        cargo.qty = 0;
        updateTrainProperties();
        Ui::WindowManager::invalidate(Ui::WindowType::vehicle, enumValue(id));
        return true;
    }

    void VehicleHead::beginLoading()
    {
        status = Status::loading;
        cargoTransferTimeout = 10;

        Vehicle train(head);
        train.cars.applyToComponents([](auto& component) { component.breakdownFlags |= BreakdownFlags::unk_0; });
    }
    // 0x004B9A2A
    void VehicleHead::updateUnloadCargo()
    {
        if (cargoTransferTimeout != 0)
        {
            cargoTransferTimeout--;
            return;
        }

        Vehicle train(head);
        for (auto& car : train.cars)
        {
            for (auto& carComponent : car)
            {
                if (carComponent.front->hasBreakdownFlags(BreakdownFlags::unk_0))
                {
                    carComponent.front->breakdownFlags &= ~BreakdownFlags::unk_0;
                    if (carComponent.front->secondaryCargo.type == 0xFF)
                    {
                        return;
                    }
                    updateUnloadCargoComponent(carComponent.front->secondaryCargo, carComponent.front);
                    return;
                }
                else if (carComponent.back->hasBreakdownFlags(BreakdownFlags::unk_0))
                {
                    carComponent.back->breakdownFlags &= ~BreakdownFlags::unk_0;
                    return;
                }
                else if (carComponent.body->hasBreakdownFlags(BreakdownFlags::unk_0))
                {
                    carComponent.body->breakdownFlags &= ~BreakdownFlags::unk_0;
                    if (carComponent.body->primaryCargo.type == 0xFF)
                    {
                        return;
                    }
                    if (updateUnloadCargoComponent(carComponent.body->primaryCargo, carComponent.back))
                    {
                        carComponent.body->updateCargoSprite();
                    }
                    return;
                }
            }
        }

        currency32_t cargoProfit = var_58;
        var_58 = 0;
        if (cargoProfit != 0)
        {
            if (aiThoughtId != 0xFF)
            {
                auto company = CompanyManager::get(owner);
                company->aiThoughts[aiThoughtId].var_80 += cargoProfit;
            }
            Vehicle2* veh2 = _vehicleUpdate_2;
            veh2->curMonthRevenue += cargoProfit;
            Vehicle1* veh1 = _vehicleUpdate_1;
            if (cargoProfit != 0)
            {
                veh1->var_48 |= Flags48::flag2;
            }

            CompanyManager::applyPaymentToCompany(owner, -cargoProfit, ExpenditureType(static_cast<uint8_t>(vehicleType) * 2));

            auto loc = train.cars.firstCar.body->position + World::Pos3{ 0, 0, 28 };
            CompanyManager::spendMoneyEffect(loc, owner, -cargoProfit);

            Audio::playSound(Audio::SoundId::income, loc);
        }

        beginLoading();
    }

    // 0x004BA19D
    bool VehicleHead::updateLoadCargoComponent(VehicleCargo& cargo, VehicleBogie* bogie)
    {
        if (cargo.maxQty == 0)
        {
            return false;
        }
        if (stationId == StationId::null)
        {
            return false;
        }

        uint8_t loadingModifier = getLoadingModifier(bogie);

        auto* station = StationManager::get(stationId);
        auto orders = getCurrentOrders();
        if (cargo.qty == 0)
        {
            // bitmask of cargo to wait for
            uint32_t cargoToWaitFor = 0;
            for (auto& order : orders)
            {
                auto* waitFor = order.as<OrderWaitFor>();
                if (waitFor != nullptr)
                {
                    cargoToWaitFor |= (1 << waitFor->getCargo());
                }
                if (order.hasFlags(OrderFlags::IsRoutable))
                {
                    break;
                }
            }
            // bitmask of all cargo from orders
            uint32_t allPossibleCargoToWaitFor = 0;
            for (auto& order : orders)
            {
                auto* waitFor = order.as<OrderWaitFor>();
                if (waitFor != nullptr)
                {
                    allPossibleCargoToWaitFor |= (1 << waitFor->getCargo());
                }
            }
            if (allPossibleCargoToWaitFor == 0)
            {
                allPossibleCargoToWaitFor = 0xFFFFFFFF;
            }

            auto acceptedCargo = cargo.acceptedTypes;
            uint8_t chosenCargo = 0xFF;
            uint16_t highestQty = 0;
            for (; acceptedCargo != 0;)
            {
                auto possibleCargo = Numerics::bitScanForward(acceptedCargo);
                acceptedCargo &= ~(1 << possibleCargo);

                if (!(allPossibleCargoToWaitFor & (1 << possibleCargo)))
                {
                    continue;
                }
                if (cargoToWaitFor & (1 << possibleCargo))
                {
                    chosenCargo = possibleCargo;
                    highestQty = std::numeric_limits<uint16_t>::max();
                }
                else
                {
                    if (highestQty < station->cargoStats[possibleCargo].quantity)
                    {
                        highestQty = station->cargoStats[possibleCargo].quantity;
                        chosenCargo = possibleCargo;
                    }
                }
            }
            if (highestQty != 0)
            {
                cargo.type = chosenCargo;
            }
        }

        for (auto& order : orders)
        {
            if (!order.hasFlags(OrderFlags::HasCargo))
            {
                break;
            }
            auto* unloadAll = order.as<OrderUnloadAll>();
            if (unloadAll != nullptr)
            {
                if (unloadAll->getCargo() == cargo.type)
                {
                    return false;
                }
            }
        }

        if (cargo.qty == cargo.maxQty)
        {
            return false;
        }

        auto* cargoObj = ObjectManager::get<CargoObject>(cargo.type);
        auto& stationCargo = station->cargoStats[cargo.type];
        auto qtyTransferred = std::min<uint16_t>(cargo.maxQty - cargo.qty, stationCargo.quantity);
        cargoTransferTimeout = static_cast<uint16_t>(std::min<uint32_t>((cargoObj->cargoTransferTime * qtyTransferred * loadingModifier) / 256, std::numeric_limits<uint16_t>::max()));

        if (stationCargo.quantity != 0)
        {
            if (cargo.qty == 0)
            {
                cargo.townFrom = stationCargo.origin;
                cargo.numDays = stationCargo.enrouteAge;
            }
            else
            {
                cargo.numDays = std::max(cargo.numDays, stationCargo.enrouteAge);

                auto* cargoSourceStation = StationManager::get(stationCargo.origin);
                auto stationLoc = World::Pos2{ station->x, station->y };
                auto cargoSourceLoc = World::Pos2{ cargoSourceStation->x, cargoSourceStation->y };

                auto stationSourceDistance = Math::Vector::distance2D(stationLoc, cargoSourceLoc);

                auto* sourceStation = StationManager::get(cargo.townFrom);
                auto sourceLoc = World::Pos2{ sourceStation->x, sourceStation->y };
                auto cargoSourceDistance = Math::Vector::distance2D(stationLoc, sourceLoc);
                if (cargoSourceDistance >= stationSourceDistance)
                {
                    cargo.townFrom = stationCargo.origin;
                }
            }
        }

        stationCargo.quantity -= qtyTransferred;
        station->updateCargoDistribution();
        cargo.qty += qtyTransferred;
        const uint8_t typeAgeMap[] = { 0, 5, 3, 2, 0, 0 };
        stationCargo.age = std::min(stationCargo.age, typeAgeMap[static_cast<uint8_t>(vehicleType)]);

        station->var_3B0 = 0;
        station->flags |= StationFlags::flag_7;
        Vehicle train(head);
        auto vehMaxSpeed = train.veh2->maxSpeed;
        auto carAgeFactor = static_cast<uint8_t>(std::min<uint32_t>(0xFF, (getCurrentDay() - bogie->creationDay) / 256));

        if ((stationCargo.flags & StationCargoStatsFlags::flag2) != StationCargoStatsFlags::none)
        {
            stationCargo.vehicleSpeed = (stationCargo.vehicleSpeed + vehMaxSpeed) / 2;
            stationCargo.vehicleAge = (stationCargo.vehicleAge + carAgeFactor) / 2;
        }
        else
        {
            stationCargo.flags |= StationCargoStatsFlags::flag2;
            stationCargo.vehicleSpeed = vehMaxSpeed;
            stationCargo.vehicleAge = carAgeFactor;
        }

        auto* company = CompanyManager::get(owner);
        company->var_49C |= 1 << cargo.type;
        updateTrainProperties();
        Ui::WindowManager::invalidate(Ui::WindowType::vehicle, enumValue(id));
        return true;
    }

    // 0x004BA142 returns false when loaded
    bool VehicleHead::updateLoadCargo()
    {
        if (cargoTransferTimeout != 0)
        {
            cargoTransferTimeout--;
            return true;
        }

        Vehicle train(head);
        for (auto& car : train.cars)
        {
            for (auto& carComponent : car)
            {
                if (carComponent.front->hasBreakdownFlags(BreakdownFlags::unk_0))
                {
                    carComponent.front->breakdownFlags &= ~BreakdownFlags::unk_0;
                    if (carComponent.front->secondaryCargo.type == 0xFF)
                    {
                        return true;
                    }
                    updateLoadCargoComponent(carComponent.front->secondaryCargo, carComponent.front);
                    return true;
                }
                else if (carComponent.back->hasBreakdownFlags(BreakdownFlags::unk_0))
                {
                    carComponent.back->breakdownFlags &= ~BreakdownFlags::unk_0;
                    return true;
                }
                else if (carComponent.body->hasBreakdownFlags(BreakdownFlags::unk_0))
                {
                    carComponent.body->breakdownFlags &= ~BreakdownFlags::unk_0;
                    if (carComponent.body->primaryCargo.type == 0xFF)
                    {
                        return true;
                    }
                    if (updateLoadCargoComponent(carComponent.body->primaryCargo, carComponent.back))
                    {
                        carComponent.body->updateCargoSprite();
                    }
                    return true;
                }
            }
        }

        auto orders = getCurrentOrders();
        for (auto& order : orders)
        {
            if (!order.hasFlags(OrderFlags::HasCargo))
            {
                currentOrder = order.getOffset() - orderTableOffset;
                Ui::WindowManager::sub_4B93A5(enumValue(id));
                break;
            }
            auto* waitFor = order.as<OrderWaitFor>();
            if (waitFor == nullptr)
            {
                continue;
            }

            bool cantWait = false;
            for (auto& car : train.cars)
            {
                for (auto& carComponent : car)
                {
                    if (carComponent.front->secondaryCargo.type == waitFor->getCargo() && carComponent.front->secondaryCargo.maxQty != carComponent.front->secondaryCargo.qty)
                    {
                        if (!hasBreakdownFlags(BreakdownFlags::unk_0))
                        {
                            beginLoading();
                            return true;
                        }
                        if (owner == CompanyManager::getControllingId())
                        {
                            MessageManager::post(MessageType::cantWaitForFullLoad, owner, enumValue(id), enumValue(stationId));
                        }
                        cantWait = true;
                        break;
                    }
                    if (carComponent.body->primaryCargo.type == waitFor->getCargo() && carComponent.body->primaryCargo.maxQty != carComponent.body->primaryCargo.qty)
                    {
                        if (!hasBreakdownFlags(BreakdownFlags::unk_0))
                        {
                            beginLoading();
                            return true;
                        }
                        if (owner == CompanyManager::getControllingId())
                        {
                            MessageManager::post(MessageType::cantWaitForFullLoad, owner, enumValue(id), enumValue(stationId));
                        }
                        cantWait = true;
                        break;
                    }
                }
                if (cantWait)
                {
                    break;
                }
            }
        }
        return false;
    }

    // 0x004BAC74
    void VehicleHead::beginNewJourney()
    {
        // Set initial position for updateLastJourneyAverageSpeed
        Vehicle train(head);
        journeyStartTicks = ScenarioManager::getScenarioTicks();
        journeyStartPos = Pos2(train.veh2->position);
        breakdownFlags |= BreakdownFlags::journeyStarted;
    }

    // 0x004707C0
    void VehicleHead::advanceToNextRoutableOrder()
    {
        if (sizeOfOrderTable == 1)
        {
            return;
        }
        OrderRingView orders(orderTableOffset, currentOrder);
        for (auto& order : orders)
        {
            if (order.hasFlags(OrderFlags::IsRoutable))
            {
                auto newOrder = order.getOffset() - orderTableOffset;
                if (newOrder != currentOrder)
                {
                    currentOrder = newOrder;
                    Ui::WindowManager::sub_4B93A5(enumValue(id));
                }
                return;
            }
        }
    }

    // 0x00427BF2
    Status VehicleHead::sub_427BF2()
    {
        return stationId == StationId::null ? Status::travelling : Status::approaching;
    }

    // 0x0042843E
    void VehicleHead::produceLeavingDockSound()
    {
        Vehicle train(head);
        const auto* vehObj = train.cars.firstCar.body->getObject();
        if (vehObj != nullptr && vehObj->numStartSounds != 0)
        {
            auto randSoundIndex = gPrng1().randNext((vehObj->numStartSounds & NumStartSounds::kMask) - 1);
            auto randSoundId = Audio::makeObjectSoundId(vehObj->startSounds[randSoundIndex]);
            Vehicle2* veh2 = _vehicleUpdate_2;
            Audio::playSound(randSoundId, veh2->position + World::Pos3{ 0, 0, 22 }, 0, 22050);
        }
    }

    struct NearbyBoats
    {
        std::array<std::array<bool, 16>, 16> searchResult; // 0x00525BEC
        World::TilePos2 startTile;                         // 0x00525BE8
    };

    // 0x00427F1C
    static NearbyBoats findNearbyTilesWithBoats(const World::Pos2 pos)
    {
        const auto tilePosA = World::toTileSpace(pos) - World::TilePos2(7, 7);
        const auto tilePosB = World::toTileSpace(pos) + World::TilePos2(7, 7);

        NearbyBoats res{};
        res.startTile = tilePosA;

        for (const auto& tileLoc : getClampedRange(tilePosA, tilePosB))
        {
            for (auto* entity : EntityManager::EntityTileList(World::toWorldSpace(tileLoc)))
            {
                auto* vehicleEntity = entity->asBase<VehicleBase>();
                if (vehicleEntity == nullptr)
                {
                    continue;
                }
                if (vehicleEntity->getTransportMode() != TransportMode::water)
                {
                    continue;
                }
                if (vehicleEntity->getSubType() != VehicleEntityType::body_start)
                {
                    continue;
                }
                const auto resultLoc = tileLoc - res.startTile;
                res.searchResult[resultLoc.x][resultLoc.y] = true;
            }
        }
        return res;
    }

    // 0x00428379
    static std::optional<WaterPathingResult> getDockTargetFromStation(StationId stationId)
    {
        auto* station = StationManager::get(stationId);
        for (auto i = 0U; i < station->stationTileSize; ++i)
        {
            // Remember z needs floored
            const auto& pos = station->stationTiles[i];

            StationElement* elStation = [&pos]() -> World::StationElement* {
                auto tile = TileManager::get(pos);
                for (auto& el : tile)
                {
                    auto* elStation = el.as<StationElement>();
                    if (elStation == nullptr)
                    {
                        continue;
                    }
                    if (elStation->isGhost() || elStation->isAiAllocated())
                    {
                        continue;
                    }
                    if (elStation->baseZ() != pos.z / World::kSmallZStep)
                    {
                        continue;
                    }
                    return elStation;
                }
                return nullptr;
            }();
            if (elStation == nullptr)
            {
                continue;
            }
            if (elStation->stationType() != StationType::docks)
            {
                continue;
            }
            if (elStation->isFlag6())
            {
                continue;
            }

            const auto* dockObj = ObjectManager::get<DockObject>(elStation->objectId());
            const auto boatPos = Math::Vector::rotate(dockObj->boatPosition, elStation->rotation()) + pos;
            auto dockTarget = WaterPathingResult(
                boatPos + World::Pos2{ 32, 32 },
                stationId,
                World::Pos3{ pos.x, pos.y, Numerics::floor2(pos.z, 4) });
            return dockTarget;
        }
        return std::nullopt;
    }

    struct PathFindingResult
    {
        uint16_t bestScore;
        uint8_t cost;

        constexpr auto operator<=>(const PathFindingResult& rhs) const = default;
    };

    // 0x00428237
    static PathFindingResult waterPathfindToTarget(const World::TilePos2 tilePos, const MicroZ waterMicroZ, const World::TilePos2 targetOrderPos, const NearbyBoats& nearbyVehicles, uint8_t cost, const PathFindingResult& bestResult)
    {
        PathFindingResult result = bestResult;
        if (!validCoords(tilePos))
        {
            return result;
        }
        auto tile = TileManager::get(tilePos);
        auto* elSurface = tile.surface();
        if (elSurface->water() != waterMicroZ)
        {
            return result;
        }
        if (!elSurface->isLast())
        {
            auto* elObsticle = elSurface->next();
            if (elObsticle != nullptr && !elObsticle->isGhost() && !elObsticle->isAiAllocated())
            {
                if (elObsticle->baseZ() / kMicroToSmallZStep - waterMicroZ < 1)
                {
                    return result;
                }
            }
        }

        const auto nearbyIndex = tilePos - nearbyVehicles.startTile;
        // Vanilla made a mistake here so we only check nearby tiles if in range
        // TODO: When we diverge just change the cost check to >= 6 or increase the search result to 18x18
        if (nearbyIndex.x >= 0 && nearbyIndex.x < 16 && nearbyIndex.y >= 0 && nearbyIndex.y < 16)
        {
            if (nearbyVehicles.searchResult[nearbyIndex.x][nearbyIndex.y])
            {
                return result;
            }
        }
        auto distToTarget = toWorldSpace(tilePos - targetOrderPos);
        distToTarget.x = std::abs(distToTarget.x);
        distToTarget.y = std::abs(distToTarget.y);
        // Lower is better
        const uint16_t score = std::max(distToTarget.x, distToTarget.y) + std::min(distToTarget.x, distToTarget.y) / 16;
        auto newResult = PathFindingResult{ score, cost };
        result = std::min(result, newResult);
        if (score != 0)
        {
            if (cost >= 7)
            {
                return result;
            }
            cost++;
            for (auto i = 0U; i < 4; ++i)
            {
                result = waterPathfindToTarget(tilePos + toTileSpace(kRotationOffset[i]), waterMicroZ, targetOrderPos, nearbyVehicles, cost, result);
            }
        }
        return result;
    }

    // 0x00427FC9
    static WaterPathingResult waterPathfind(const VehicleHead& head)
    {
        const auto nearbyVehicles = findNearbyTilesWithBoats(head.position);

        auto orders = head.getCurrentOrders();
        auto curOrder = orders.begin();
        auto* stationOrder = curOrder->as<OrderStation>();
        auto* routeOrder = curOrder->as<OrderRouteWaypoint>();

        std::optional<WaterPathingResult> dockRes = std::nullopt;
        // 0x00525BC6
        World::TilePos2 targetOrderPos{};
        if (routeOrder != nullptr)
        {
            targetOrderPos = toTileSpace(routeOrder->getWaypoint());
        }
        else if (stationOrder != nullptr)
        {
            auto targetStationId = stationOrder->getStation();
            dockRes = getDockTargetFromStation(targetStationId);
            if (dockRes.has_value())
            {
                targetOrderPos = toTileSpace(dockRes->headTarget);
            }
            else
            {
                auto* station = StationManager::get(targetStationId);
                targetOrderPos = toTileSpace(Pos2{ station->x, station->y });
            }
        }
        else
        {
            targetOrderPos = toTileSpace(head.position);
        }

        Vehicle train(head);
        const auto& veh2 = *train.veh2;
        // 0x00525BE3
        const uint8_t curRotation = ((veh2.spriteYaw + 7) >> 4) & 3;

        const auto initialTile = toTileSpace(head.position);
        const auto waterMicroZ = veh2.position.z / World::kMicroZStep;

        PathFindingResult bestResult{ std::numeric_limits<uint16_t>::max(), std::numeric_limits<uint8_t>::max() };
        uint8_t bestResultDirection = 0xFFU;
        for (auto i = 0U; i < 4; ++i)
        {
            const auto tilePos = initialTile + toTileSpace(kRotationOffset[i]);
            PathFindingResult initResult{ std::numeric_limits<uint16_t>::max(), std::numeric_limits<uint8_t>::max() };
            const auto pathResult = waterPathfindToTarget(tilePos, waterMicroZ, targetOrderPos, nearbyVehicles, 0, initResult);
            if (pathResult != initResult && (pathResult < bestResult || (pathResult == bestResult && i == curRotation)))
            {
                bestResult = pathResult;
                bestResultDirection = i;
            }
        }

        if (bestResultDirection == 0xFF)
        {
            return WaterPathingResult(toWorldSpace(initialTile) + World::Pos2(16, 16));
        }

        if (dockRes.has_value() && bestResult == PathFindingResult{ 0, 0 })
        {
            return dockRes.value();
        }
        else
        {
            const auto targetPos = toWorldSpace(initialTile) + kRotationOffset[bestResultDirection] + World::Pos2(16, 16);
            return WaterPathingResult(targetPos);
        }
    }

    // 0x0042750E
    void VehicleHead::produceTouchdownAirportSound()
    {
        Vehicle train(head);
        const auto* vehObj = train.cars.firstCar.body->getObject();
        if (vehObj != nullptr && vehObj->numStartSounds != 0)
        {

            auto randSoundIndex = gPrng1().randNext((vehObj->numStartSounds & NumStartSounds::kMask) - 1);
            auto randSoundId = Audio::makeObjectSoundId(vehObj->startSounds[randSoundIndex]);

            Vehicle2* veh2 = _vehicleUpdate_2;
            Audio::playSound(randSoundId, veh2->position + World::Pos3{ 0, 0, 22 }, 0, 22050);
        }
    }

    // 0x004AD778
    void VehicleHead::sub_4AD778()
    {
        registers regs;
        regs.esi = X86Pointer(this);
        call(0x004AD778, regs);
    }

    // 0x004AA625
    void VehicleHead::landCrashedUpdate()
    {
        VehicleBase* currentVehicle = this;
        while (currentVehicle != nullptr)
        {
            switch (currentVehicle->getSubType())
            {
                case VehicleEntityType::head:
                    currentVehicle->asVehicleHead()->updateSegmentCrashed();
                    break;
                case VehicleEntityType::bogie:
                    currentVehicle->asVehicleBogie()->updateSegmentCrashed();
                    break;
                case VehicleEntityType::body_start:
                case VehicleEntityType::body_continued:
                    currentVehicle->asVehicleBody()->updateSegmentCrashed();
                    break;
                case VehicleEntityType::vehicle_1:
                case VehicleEntityType::vehicle_2:
                case VehicleEntityType::tail:
                    break;
            }

            currentVehicle = currentVehicle->nextVehicleComponent();
        }
    }

    // 0x004AA64B
    void VehicleHead::updateSegmentCrashed()
    {
        Vehicle train(head);
        _vehicleUpdate_head = this;
        _vehicleUpdate_frontBogie = reinterpret_cast<VehicleBogie*>(0xFFFFFFFF);
        _vehicleUpdate_backBogie = reinterpret_cast<VehicleBogie*>(0xFFFFFFFF);

        _vehicleUpdate_1 = train.veh1;
        _vehicleUpdate_2 = train.veh2;
    }

    // 0x004ACEE7
    std::tuple<uint8_t, uint8_t, StationId> VehicleHead::sub_4ACEE7(uint32_t unk1, uint32_t var_113612C)
    {
        registers regs;
        regs.esi = X86Pointer(this);
        regs.eax = unk1;
        regs.ebx = var_113612C;
        call(0x004ACEE7, regs);
        // status, flags, stationId
        return std::make_tuple(static_cast<uint8_t>(regs.al), static_cast<uint8_t>(regs.ah), static_cast<StationId>(regs.bp));
    }

    // 0x004AC1C2
    bool VehicleHead::sub_4AC1C2()
    {
        const auto [nextPos, rotation] = World::Track::getTrackConnectionEnd(getTrackLoc(), trackAndDirection.track._data);
        auto tc = World::Track::getTrackConnections(nextPos, rotation, owner, trackType, 0, 0);
        if (tc.connections.size() != 1)
        {
            return false;
        }
        TrackAndDirection::_TrackAndDirection tad((tc.connections.front() & World::Track::AdditionalTaDFlags::basicTaDMask) >> 3, tc.connections.front() & 0x7);
        return sub_4A2A58(nextPos, tad, owner, trackType) & (1 << 1);
    }

    // 0x004AC0A3
    bool VehicleHead::opposingTrainAtSignal()
    {
        // Works out if there is an opposing train in the tile 2 ahead (i.e. waiting at a signal)
        // The train could be on any of the multiple connections on that tile.

        const auto [nextPos, rotation] = World::Track::getTrackConnectionEnd(getTrackLoc(), trackAndDirection.track._data);
        auto tc1 = World::Track::getTrackConnections(nextPos, rotation, owner, trackType, 0, 0);
        if (tc1.connections.size() != 1)
        {
            return false;
        }

        const auto [nextNextPos, nextRotation] = World::Track::getTrackConnectionEnd(nextPos, tc1.connections.front() & Track::AdditionalTaDFlags::basicTaDMask);
        auto tc2 = World::Track::getTrackConnections(nextNextPos, nextRotation, owner, trackType, 0, 0);
        if (tc2.connections.empty())
        {
            return false;
        }

        for (auto c : tc2.connections)
        {
            TrackAndDirection::_TrackAndDirection tad{ 0, 0 };
            tad._data = c & Track::AdditionalTaDFlags::basicTaDMask;
            auto& trackSize = World::TrackData::getUnkTrack(tad._data);
            auto pos = nextNextPos + trackSize.pos;
            if (trackSize.rotationEnd < 12)
            {
                pos -= World::Pos3{ kRotationOffset[trackSize.rotationEnd], 0 };
            }

            auto reverseTad = tad;
            reverseTad.setReversed(!tad.isReversed());

            for (auto* v : VehicleManager::VehicleList())
            {
                Vehicle vehicle(v->head);
                auto* veh2 = vehicle.veh2;
                if (veh2->tileX == pos.x
                    && veh2->tileY == pos.y
                    && veh2->tileBaseZ == pos.z / World::kSmallZStep
                    && veh2->trackAndDirection.track == reverseTad)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // 0x0047DFD0
    static void sub_47DFD0(VehicleHead& head, World::Pos3 pos, Track::LegacyTrackConnections& connections, bool unk)
    {
        // ROAD only
        static loco_global<World::Track::LegacyTrackConnections, 0x0113609C> _113609C;
        _113609C = connections;

        registers regs;
        regs.ax = pos.x;
        regs.cx = pos.y;
        regs.dx = pos.z | (unk ? 0x8000 : 0);
        regs.esi = X86Pointer(&head);
        call(0x0047DFD0, regs);
    }

    // 0x004AC3D3
    static void sub_4AC3D3(VehicleHead& head, World::Pos3 pos, Track::LegacyTrackConnections& connections, bool unk)
    {
        // TRACK only
        static loco_global<World::Track::LegacyTrackConnections, 0x0113609C> _113609C;
        _113609C = connections;

        registers regs;
        regs.ax = pos.x;
        regs.cx = pos.y;
        regs.dx = pos.z | (unk ? 0x8000 : 0);
        regs.esi = X86Pointer(&head);
        call(0x004AC3D3, regs);
    }

    // 0x004ACCE6
    static bool trackSub_4ACCE6(VehicleHead& head)
    {
        auto train = Vehicle(head);

        _113601A[0] = head.var_53;        // TODO: Remove after sub_4AC3D3
        _113601A[1] = train.veh1->var_49; // TODO: Remove after sub_4AC3D3
        {
            auto [nextPos, nextRotation] = Track::getTrackConnectionEnd(World::Pos3(head.tileX, head.tileY, head.tileBaseZ * World::kSmallZStep), head.trackAndDirection.track._data);
            auto tc = World::Track::getTrackConnections(nextPos, nextRotation, head.owner, head.trackType, head.var_53, train.veh1->var_49);
            if (tc.connections.empty())
            {
                return false;
            }
            Track::LegacyTrackConnections legacyTc{};
            Track::toLegacyConnections(tc, legacyTc);
            sub_4AC3D3(head, nextPos, legacyTc, false);
        }
        {
            auto tailTaD = train.tail->trackAndDirection.track._data;
            const auto& trackSize = TrackData::getUnkTrack(tailTaD);
            auto pos = World::Pos3(train.tail->tileX, train.tail->tileY, train.tail->tileBaseZ * World::kSmallZStep) + trackSize.pos;
            if (trackSize.rotationEnd < 12)
            {
                pos -= World::Pos3{ World::kRotationOffset[trackSize.rotationEnd], 0 };
            }
            tailTaD ^= (1U << 2); // Reverse
            auto [nextTailPos, nextTailRotation] = Track::getTrackConnectionEnd(pos, tailTaD);
            auto tailTc = World::Track::getTrackConnections(nextTailPos, nextTailRotation, train.tail->owner, train.tail->trackType, head.var_53, train.veh1->var_49);

            if (tailTc.connections.empty())
            {
                return false;
            }

            _1136458 = 0;
            Track::LegacyTrackConnections legacyTc{};
            Track::toLegacyConnections(tailTc, legacyTc);
            sub_4AC3D3(head, nextTailPos, legacyTc, true);
            return _1136458 != 0;
        }
    }

    // 0x004ACDE0
    static bool roadSub_4ACDE0(VehicleHead& head)
    {
        auto train = Vehicle(head);
        if (head.trackType != 0xFFU)
        {
            auto* roadObj = ObjectManager::get<RoadObject>(head.trackType);
            if (!roadObj->hasFlags(RoadObjectFlags::isRoad))
            {
                return false;
            }
        }

        _113601A[0] = head.var_53;        // TODO: Remove after sub_47DFD0
        _113601A[1] = train.veh1->var_49; // TODO: Remove after sub_47DFD0
        {
            auto [nextPos, nextRotation] = Track::getRoadConnectionEnd(World::Pos3(head.tileX, head.tileY, head.tileBaseZ * World::kSmallZStep), head.trackAndDirection.road._data & 0x7F);
            const auto rc = World::Track::getRoadConnections(nextPos, nextRotation, head.owner, head.trackType, head.var_53, train.veh1->var_49);
            if (rc.connections.empty())
            {
                return false;
            }
            Track::LegacyTrackConnections connections{};
            Track::toLegacyConnections(rc, connections);

            sub_47DFD0(head, nextPos, connections, false);
        }
        {
            auto tailTaD = train.tail->trackAndDirection.road._data & 0x7F;
            const auto& trackSize = TrackData::getUnkRoad(tailTaD);
            const auto pos = World::Pos3(train.tail->tileX, train.tail->tileY, train.tail->tileBaseZ * World::kSmallZStep) + trackSize.pos;
            tailTaD ^= (1U << 2); // Reverse
            auto [nextTailPos, nextTailRotation] = Track::getRoadConnectionEnd(pos, tailTaD);
            const auto tailRc = World::Track::getRoadConnections(nextTailPos, nextTailRotation, train.tail->owner, train.tail->trackType, head.var_53, train.veh1->var_49);

            if (tailRc.connections.empty())
            {
                return false;
            }

            Track::LegacyTrackConnections tailConnections{};
            Track::toLegacyConnections(tailRc, tailConnections);
            _1136458 = 0;
            sub_47DFD0(head, nextTailPos, tailConnections, true);
            return _1136458 != 0;
        }
    }

    // 0x004ACCDC
    bool VehicleHead::sub_4ACCDC()
    {
        if (mode == TransportMode::rail)
        {
            return trackSub_4ACCE6(*this);
        }
        else
        {
            return roadSub_4ACDE0(*this);
        }
    }

    // 0x004AD93A
    void VehicleHead::sub_4AD93A()
    {
        registers regs;
        regs.esi = X86Pointer(this);
        call(0x004AD93A, regs);
    }

    static StationId tryFindStationAt(VehicleBogie* bogie)
    {
        auto direction = bogie->trackAndDirection.track.cardinalDirection();
        auto trackId = bogie->trackAndDirection.track.id();

        auto tile = TileManager::get(World::Pos2{ bogie->tileX, bogie->tileY });
        auto* elStation = tile.trainStation(trackId, direction, bogie->tileBaseZ);
        if (elStation == nullptr)
        {
            return StationId::null;
        }
        if (elStation->isAiAllocated() || elStation->isGhost())
        {
            return StationId::null;
        }

        return elStation->stationId();
    }

    // 0x004B6669
    char* VehicleHead::generateCargoTotalString(char* buffer)
    {
        CargoTotalArray cargoTotals{};
        Vehicles::Vehicle train(head);
        for (const auto& car : train.cars)
        {
            auto front = car.front;
            auto body = car.body;

            if (front->secondaryCargo.type != 0xFF)
            {
                cargoTotals[front->secondaryCargo.type] += front->secondaryCargo.qty;
            }
            if (body->primaryCargo.type != 0xFF)
            {
                cargoTotals[body->primaryCargo.type] += body->primaryCargo.qty;
            }
        }

        return cargoLUTToString(cargoTotals, buffer);
    }

    char* VehicleHead::generateCargoCapacityString(char* buffer)
    {
        CargoTotalArray cargoTotals{};
        Vehicles::Vehicle train(head);
        for (const auto& car : train.cars)
        {
            auto front = car.front;
            auto body = car.body;

            if (front->secondaryCargo.type != 0xFF)
            {
                cargoTotals[front->secondaryCargo.type] += front->secondaryCargo.maxQty;
            }
            if (body->primaryCargo.type != 0xFF)
            {
                cargoTotals[body->primaryCargo.type] += body->primaryCargo.maxQty;
            }
        }

        return cargoLUTToString(cargoTotals, buffer);
    }

    char* VehicleHead::cargoLUTToString(CargoTotalArray& cargoTotals, char* buffer)
    {
        bool hasCargo = false;
        for (size_t cargoType = 0; cargoType < ObjectManager::getMaxObjects(ObjectType::cargo); ++cargoType)
        {
            auto cargoTotal = cargoTotals[cargoType];
            if (cargoTotal == 0)
            {
                continue;
            }

            // On all but first loop insert a ", "
            if (hasCargo)
            {
                *buffer++ = ',';
                *buffer++ = ' ';
            }
            hasCargo = true;
            auto cargoObj = ObjectManager::get<CargoObject>(cargoType);
            auto unitNameFormat = cargoTotal == 1 ? cargoObj->unitNameSingular : cargoObj->unitNamePlural;
            FormatArguments args{};
            args.push(cargoTotal);
            buffer = StringManager::formatString(buffer, unitNameFormat, args);
        }

        if (!hasCargo)
        {
            buffer = StringManager::formatString(buffer, StringIds::cargo_empty_2);
        }

        return buffer;
    }

    // 0x004BABAD
    // Manual control has much broader detection of when at a station
    // it is defined as stationary with at least one bogie at the station
    StationId VehicleHead::manualFindTrainStationAtLocation()
    {
        Vehicle train(head);
        for (auto& car : train.cars)
        {
            for (auto& component : car)
            {
                auto foundStation = tryFindStationAt(component.front);
                if (foundStation != StationId::null)
                {
                    return foundStation;
                }
                foundStation = tryFindStationAt(component.back);
                if (foundStation != StationId::null)
                {
                    return foundStation;
                }
            }
        }
        return StationId::null;
    }

    // 0x004ADB47
    void VehicleHead::sub_4ADB47(bool unk)
    {
        registers regs;
        regs.esi = X86Pointer(this);
        regs.eax = unk ? 1 : 0;
        call(0x004ADB47, regs);
    }

    // 0x004BADE4
    bool VehicleHead::isOnExpectedRoadOrTrack()
    {
        Vehicle train(head);
        Vehicle2* veh = train.veh2;
        Pos2 loc = {
            veh->tileX,
            veh->tileY
        };
        auto baseZ = veh->tileBaseZ;

        auto tile = TileManager::get(loc);
        if (veh->mode == TransportMode::road)
        {
            for (auto& el : tile)
            {
                auto* elRoad = el.as<RoadElement>();
                if (elRoad == nullptr)
                {
                    continue;
                }

                auto heightDiff = std::abs(elRoad->baseZ() - baseZ);
                if (heightDiff > 4)
                {
                    continue;
                }

                if (elRoad->isGhost() || elRoad->isAiAllocated())
                {
                    continue;
                }

                if (elRoad->roadId() != veh->trackAndDirection.road.id())
                {
                    continue;
                }

                return true;
            }
            return false;
        }
        else
        {
            for (auto& el : tile)
            {
                auto* elTrack = el.as<TrackElement>();
                if (elTrack == nullptr)
                {
                    continue;
                }

                auto heightDiff = std::abs(elTrack->baseZ() - baseZ);
                if (heightDiff > 4)
                {
                    continue;
                }

                if (elTrack->isGhost() || elTrack->isAiAllocated())
                {
                    continue;
                }

                if (elTrack->rotation() != veh->trackAndDirection.track.cardinalDirection())
                {
                    continue;
                }

                if (elTrack->trackId() != veh->trackAndDirection.track.id())
                {
                    continue;
                }

                return true;
            }
            return false;
        }
    }

    // 0x004BA7FC
    void IncomeStats::beginNewIncome()
    {
        day = getCurrentDay();
        std::fill(std::begin(cargoTypes), std::end(cargoTypes), 0xFF);
    }

    // 0x4BA817
    // Returns false if stats were not updated
    bool IncomeStats::addToStats(uint8_t cargoType, uint16_t cargoQty, uint16_t cargoDist, uint8_t cargoAge, currency32_t profit)
    {
        for (auto i = 0; i < 4; ++i)
        {
            if (cargoTypes[i] != cargoType)
            {
                continue;
            }
            if (cargoDistances[i] != cargoDist)
            {
                continue;
            }
            cargoQtys[i] += cargoQty;
            cargoProfits[i] += profit;
            cargoAges[i] = std::max(cargoAge, cargoAges[i]);
            return true;
        }

        for (auto i = 0; i < 4; ++i)
        {
            if (cargoTypes[i] != 0xFF)
            {
                continue;
            }

            cargoTypes[i] = cargoType;
            cargoDistances[i] = cargoDist;
            cargoQtys[i] = cargoQty;
            cargoAges[i] = cargoAge;
            cargoProfits[i] = profit;
            return true;
        }
        return false;
    }

    // 0x004BA7C7
    void VehicleHead::updateLastIncomeStats(uint8_t cargoType, uint16_t cargoQty, uint16_t cargoDist, uint8_t cargoAge, currency32_t profit)
    {
        Vehicle train(head);
        if (cargoQty == 0)
        {
            return;
        }
        if (cargoType == 0xFF)
        {
            return;
        }

        auto* veh1 = train.veh1;
        if ((veh1->var_48 & Flags48::flag2) != Flags48::none)
        {
            veh1->var_48 &= ~Flags48::flag2;
            veh1->lastIncome.beginNewIncome();
        }

        if (veh1->lastIncome.addToStats(cargoType, cargoQty, cargoDist, cargoAge, profit))
        {
            Ui::WindowManager::invalidate(Ui::WindowType::vehicle, enumValue(id));
        }
    }

    // 0x004B82AE
    void VehicleHead::calculateRefundCost()
    {
        Vehicle train(head);

        totalRefundCost = std::accumulate(train.cars.begin(), train.cars.end(), 0, [](currency32_t total, const Car& car) {
            return total + car.front->refundCost;
        });
    }

    template<typename T>
    static void setTrainModFlags(Vehicle& train, T& trackRoadObj)
    {
        bool canRoll = true;
        uint32_t roadMods = 0;
        uint32_t rackRailMods = 0;
        for (auto& car : train.cars)
        {
            auto* vehicleObj = ObjectManager::get<VehicleObject>(car.front->objectId);
            if (vehicleObj->bodySprites[0].numRollFrames == 1)
            {
                canRoll = false;
            }
            if (vehicleObj->hasFlags(VehicleObjectFlags::rackRail))
            {
                for (auto i = 0U; i < trackRoadObj.numMods; ++i)
                {
                    if (trackRoadObj.mods[i] == vehicleObj->rackRailType)
                    {
                        rackRailMods |= (1U << i);
                        break;
                    }
                }
            }

            for (auto i = 0U; i < vehicleObj->numTrackExtras; ++i)
            {
                const auto mod = vehicleObj->requiredTrackExtras[i];
                for (auto j = 0U; j < trackRoadObj.numMods; ++j)
                {
                    if (trackRoadObj.mods[j] == mod)
                    {
                        roadMods |= (1U << j);
                        break;
                    }
                }
            }
        }
        train.head->var_53 = roadMods;
        train.veh1->var_49 = rackRailMods;
        train.head->var_38 &= ~Flags38::fasterAroundCurves;
        if (canRoll)
        {
            train.head->var_38 |= Flags38::fasterAroundCurves;
        }
    }

    // 0x004B7CC3
    void VehicleHead::updateTrainProperties()
    {
        Vehicle train(head);
        if (mode == TransportMode::road)
        {
            // 0x004B7E01
            uint8_t roadObjId = trackType;
            if (roadObjId == 0xFF)
            {
                roadObjId = getGameState().lastTrackTypeOption;
            }
            auto* roadObj = ObjectManager::get<RoadObject>(roadObjId);
            setTrainModFlags(train, *roadObj);
        }
        else
        {
            // 0x004B7CCE
            auto* trackObj = ObjectManager::get<TrackObject>(trackType);

            setTrainModFlags(train, *trackObj);
        }

        // 0x004B7F3B
        uint32_t totalTrainPower = 0;
        // edx
        uint32_t totalTrainWeight = 0;
        Speed16 maxTrainSpeed = kSpeed16Max;
        Speed16 rackMaxTrainSpeed = kSpeed16Max;
        uint32_t totalTrainAcceptedCargoTypes = 0;
        // 0x011360F0
        uint32_t earliestPoweredCreationDay = 0xFFFF'FFFFU;
        // 0x011360F4
        uint32_t earliestCreationDay = 0xFFFF'FFFFU;
        for (auto& car : train.cars)
        {
            auto* vehicleObj = ObjectManager::get<VehicleObject>(car.front->objectId);
            totalTrainPower += vehicleObj->power;
            if (vehicleObj->power != 0)
            {
                earliestPoweredCreationDay = std::min(earliestPoweredCreationDay, car.front->creationDay);
            }
            earliestCreationDay = std::min(earliestCreationDay, car.front->creationDay);

            uint16_t totalCarWeight = vehicleObj->weight;
            totalTrainAcceptedCargoTypes |= car.front->secondaryCargo.acceptedTypes;
            if (car.front->secondaryCargo.type != 0xFFU)
            {
                auto* cargoObj = ObjectManager::get<CargoObject>(car.front->secondaryCargo.type);
                totalCarWeight += (cargoObj->unitWeight * car.front->secondaryCargo.qty) / 256;
            }

            // Back doesn't have cargo but lets match vanilla
            totalTrainAcceptedCargoTypes |= car.back->secondaryCargo.acceptedTypes;
            if (car.back->secondaryCargo.type != 0xFFU)
            {
                auto* cargoObj = ObjectManager::get<CargoObject>(car.back->secondaryCargo.type);
                totalCarWeight += (cargoObj->unitWeight * car.back->secondaryCargo.qty) / 256;
            }

            totalTrainAcceptedCargoTypes |= car.body->primaryCargo.acceptedTypes;
            if (car.body->primaryCargo.type != 0xFFU)
            {
                auto* cargoObj = ObjectManager::get<CargoObject>(car.body->primaryCargo.type);
                totalCarWeight += (cargoObj->unitWeight * car.body->primaryCargo.qty) / 256;
            }

            car.front->totalCarWeight = totalCarWeight;
            totalTrainWeight += totalCarWeight;

            maxTrainSpeed = std::min(maxTrainSpeed, vehicleObj->speed);
            rackMaxTrainSpeed = std::min(rackMaxTrainSpeed, vehicleObj->speed);
            if (vehicleObj->hasFlags(VehicleObjectFlags::rackRail))
            {
                rackMaxTrainSpeed = std::min(rackMaxTrainSpeed, vehicleObj->rackSpeed);
            }
            if (vehicleObj->mode == TransportMode::air)
            {
                rackMaxTrainSpeed = vehicleObj->rackSpeed;
            }
        }
        trainAcceptedCargoTypes = totalTrainAcceptedCargoTypes;
        auto createDay = earliestPoweredCreationDay;
        if (earliestPoweredCreationDay == 0xFFFF'FFFF)
        {
            createDay = earliestCreationDay;
            if (earliestCreationDay == 0xFFFF'FFFF)
            {
                createDay = getCurrentDay();
            }
        }
        train.veh1->dayCreated = createDay;
        train.veh2->totalPower = static_cast<uint16_t>(std::min<uint32_t>(totalTrainPower, 0xFFFFU));
        train.veh2->totalWeight = static_cast<uint16_t>(std::min<uint32_t>(totalTrainWeight, 0xFFFFU));
        train.veh2->maxSpeed = maxTrainSpeed;
        train.veh2->rackRailMaxSpeed = rackMaxTrainSpeed;
        train.veh2->var_4F = 0xFFU;

        uint16_t frontSoundingObjId = 0xFFFFU;
        uint16_t backSoundingObjId = 0xFFFFU;
        for (const auto& car : train.cars)
        {
            auto* vehicleObj = ObjectManager::get<VehicleObject>(car.body->objectId);
            if (vehicleObj->drivingSoundType == DrivingSoundType::none)
            {
                continue;
            }
            if (frontSoundingObjId == 0xFFFFU)
            {
                frontSoundingObjId = car.body->objectId;
            }
            if (frontSoundingObjId != car.body->objectId)
            {
                backSoundingObjId = car.body->objectId;
            }
        }

        // This could happen if the train flips direction
        if (frontSoundingObjId != train.veh2->objectId
            && frontSoundingObjId == train.tail->objectId)
        {
            std::swap(train.veh2->drivingSoundId, train.tail->drivingSoundId);
            std::swap(train.veh2->drivingSoundVolume, train.tail->drivingSoundVolume);
            std::swap(train.veh2->drivingSoundFrequency, train.tail->drivingSoundFrequency);
            std::swap(train.veh2->soundFlags, train.tail->soundFlags);
        }
        train.veh2->objectId = frontSoundingObjId;
        train.tail->objectId = backSoundingObjId;

        calculateRefundCost();
        recalculateTrainMinReliability(*this);
    }

    OrderRingView Vehicles::VehicleHead::getCurrentOrders() const
    {
        return OrderRingView(orderTableOffset, currentOrder);
    }

    // 0x004B0BDD
    bool Vehicles::VehicleHead::canBeModified() const
    {
        switch (this->status)
        {
            case Status::crashed:
                GameCommands::setErrorText(StringIds::vehicle_has_crashed);
                return false;
            case Status::stuck:
                GameCommands::setErrorText(StringIds::vehicle_is_stuck);
                return false;
            case Status::brokenDown:
                GameCommands::setErrorText(StringIds::vehicle_has_broken_down);
                return false;
            default:
            {
                Vehicle train(head);
                if (this->vehicleType == VehicleType::aircraft || this->vehicleType == VehicleType::ship)
                {
                    if (train.veh2->has73Flags(Flags73::isBrokenDown))
                    {
                        GameCommands::setErrorText(StringIds::vehicle_has_broken_down);
                        return false;
                    }

                    if (this->tileX == -1)
                    {
                        return true;
                    }

                    if (this->status != Status::loading && this->status != Status::stopped)
                    {
                        GameCommands::setErrorText(StringIds::vehicle_must_be_stopped);
                        return false;
                    }

                    if (train.veh2->currentSpeed == 0.0_mph)
                    {
                        return true;
                    }
                    GameCommands::setErrorText(StringIds::vehicle_must_be_stopped);
                    return false;
                }
                else
                {
                    if (this->tileX == -1)
                    {
                        return true;
                    }

                    if (train.veh2->currentSpeed == 0.0_mph)
                    {
                        return true;
                    }
                    if (train.veh1->var_3C <= 0x3689)
                    {
                        return true;
                    }
                    GameCommands::setErrorText(StringIds::vehicle_must_be_stopped);
                    return false;
                }
            }
        }
    }

    // 0x004B08DD
    void VehicleHead::liftUpVehicle()
    {
        if (tileX == -1)
        {
            return;
        }

        sub_4AD778();
        Vehicle train(head);
        train.applyToComponents([](auto& vehicle) {
            vehicle.var_38 |= Flags38::unk_2;
        });

        liftUpTail(*train.tail);

        // 0x004B0A9A
        train.applyToComponents([](auto& veh) {
            veh.var_38 &= ~Flags38::unk_2;
            veh.tileX = -1;
            veh.moveTo(World::Pos3(static_cast<int16_t>(0x8000), 0, 0));
        });

        for (auto& car : train.cars)
        {
            for (auto& component : car)
            {
                removeAllCargo(component);
            }
        }

        train.veh1->var_3C = 0;
        status = Status::unk_0;
        stationId = StationId::null;
    }

    // 0x004C3BA6
    currency32_t VehicleHead::calculateRunningCost() const
    {
        currency32_t totalRunCost = 0;
        const Vehicle train(head);
        for (const auto& car : train.cars)
        {
            auto* vehObj = ObjectManager::get<VehicleObject>(car.front->objectId);
            currency32_t runCost = Economy::getInflationAdjustedCost(vehObj->runCostFactor, vehObj->runCostIndex, 10);
            totalRunCost += runCost;
        }
        return totalRunCost;
    }
}
