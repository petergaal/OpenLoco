#include "Cheat.h"
#include "../CompanyManager.h"
#include "../Economy/Currency.h"
#include "../Entities/EntityManager.h"
#include "../Interop/Interop.hpp"
#include "../Map/TileManager.h"
#include "../StationManager.h"
#include "../TownManager.h"
#include "../Types.hpp"
#include "../Vehicles/Vehicle.h"
#include "GameCommands.h"

using namespace OpenLoco::Interop;

namespace OpenLoco::GameCommands
{
    namespace Cheats
    {
        static uint32_t acquireAssets(CompanyId_t targetCompanyId)
        {
            auto ourCompanyId = CompanyManager::updatingCompanyId();

            // First phase: change ownership of all tile elements that currently belong to the target company.
            for (auto& element : TileManager::getElements())
            {
                auto roadElement = element.asRoad();
                if (roadElement != nullptr)
                {
                    roadElement->setOwner(ourCompanyId);
                    continue;
                }

                auto trackElement = element.asTrack();
                if (trackElement != nullptr)
                {
                    trackElement->setOwner(ourCompanyId);
                    continue;
                }
            }

            // Second phase: change ownership of all stations that currently belong to the target company.
            for (auto& station : StationManager::stations())
            {
                if (station.empty() || station.owner != targetCompanyId)
                    continue;

                station.owner = ourCompanyId;
            }

            // Third phase: change ownership of all vehicles that currently belong to the target company.
            for (auto vehicle : EntityManager::VehicleList())
            {
                if (vehicle->owner != targetCompanyId)
                    continue;

                Vehicles::Vehicle train(vehicle);
                train.head->owner = ourCompanyId;
                train.veh1->owner = ourCompanyId;
                train.veh2->owner = ourCompanyId;
                train.tail->owner = ourCompanyId;

                for (auto& car : train.cars)
                {
                    for (auto& component : car)
                    {
                        component.front->owner = ourCompanyId;
                        component.body->owner = ourCompanyId;
                        component.back->owner = ourCompanyId;
                    }
                }
            }

            return 0;
        }

        static uint32_t addCash(currency32_t amount)
        {
            auto company = CompanyManager::getPlayerCompany();
            company->cash = company->cash + amount;
            return 0;
        }

        static uint32_t clearLoan()
        {
            auto company = CompanyManager::getPlayerCompany();
            company->current_loan = 0;
            return 0;
        }

        static uint32_t companyRatings(bool absolute, int32_t value)
        {
            auto companyId = CompanyManager::getControllingId();

            for (auto& town : TownManager::towns())
            {
                // Does this town have a rating for our company?
                if (!(town.companies_with_rating &= (1 << companyId)))
                    continue;

                int16_t newRanking{};
                if (absolute)
                {
                    newRanking = value * max_company_rating;
                }
                else
                {
                    newRanking = town.company_ratings[companyId] + max_company_rating;
                    newRanking *= 1.0f + (1.0f / value);
                    newRanking -= max_company_rating;
                }

                // Set the new rating.
                town.company_ratings[companyId] = std::clamp<int16_t>(newRanking, min_company_rating, max_company_rating);
            }

            return 0;
        }

        static uint32_t switchCompany(CompanyId_t targetCompanyId)
        {
            auto ourId = CompanyManager::getControllingId();
            auto otherId = CompanyManager::getSecondaryPlayerId();

            // Already controlling the target company?
            if (targetCompanyId == ourId)
                return 0;

            // Is the other player controlling the target company? Swap companies.
            if (targetCompanyId == otherId)
            {
                CompanyManager::setSecondaryPlayerId(ourId);
                CompanyManager::setControllingId(otherId);
                return 0;
            }

            // Change control over to the other company.
            CompanyManager::setControllingId(targetCompanyId);
            return 0;
        }

        static uint32_t toggleBankruptcy(CompanyId_t targetCompanyId)
        {
            auto company = CompanyManager::get(targetCompanyId);
            company->challenge_flags ^= CompanyFlags::bankrupt;
            return 0;
        }

        static uint32_t toggleJail(CompanyId_t targetCompanyId)
        {
            auto company = CompanyManager::get(targetCompanyId);
            company->jail_status = 30;
            return 0;
        }

        static uint32_t vehicleReliability(int32_t newReliablity)
        {
            auto ourCompanyId = CompanyManager::updatingCompanyId();

            for (auto vehicle : EntityManager::VehicleList())
            {
                if (vehicle->owner != ourCompanyId)
                    continue;

                Vehicles::Vehicle train(vehicle);
                train.veh2->reliability = newReliablity;

                // Set reliability for the first car's front bogie component.
                auto& car = *(train.cars.begin());
                auto& component = *(car.begin());
                component.front->reliability = newReliablity * 256;
            }
            return 0;
        }
    }

    static uint32_t cheat(CheatCommand command, int32_t param1, int32_t param2, int32_t param3)
    {
        switch (command)
        {
            case CheatCommand::acquireAssets:
                return Cheats::acquireAssets(param1);

            case CheatCommand::addCash:
                return Cheats::addCash(param1);

            case CheatCommand::clearLoan:
                return Cheats::clearLoan();

            case CheatCommand::companyRatings:
                return Cheats::companyRatings(param1, param2);

            case CheatCommand::switchCompany:
                return Cheats::switchCompany(param1);

            case CheatCommand::toggleBankruptcy:
                return Cheats::toggleBankruptcy(param1);

            case CheatCommand::toggleJail:
                return Cheats::toggleJail(param1);

            case CheatCommand::vehicleReliability:
                return Cheats::vehicleReliability(param1);

            default:
                break;
        }

        return 0;
    }

    void cheat(registers& regs)
    {
        regs.ebx = cheat(CheatCommand(regs.eax), regs.ebx, regs.ecx, regs.edx);
    }
}
