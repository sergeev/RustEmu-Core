/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2010-2012 /dev/rsa for MangosR2 <http://github.com/MangosR2>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef MANGOSSERVER_VEHICLE_H
#define MANGOSSERVER_VEHICLE_H

#include "Common.h"
#include "ObjectGuid.h"
#include "Creature.h"
#include "Unit.h"
#include "SharedDefines.h"
#include "TransportSystem.h"

#define SPELL_RIDE_VEHICLE_HARDCODED 46598

struct VehicleEntry;

struct VehicleSeat
{
    VehicleSeat(VehicleSeatEntry const* pSeatInfo = nullptr) : seatInfo(pSeatInfo), passengerGuid(ObjectGuid()), b_dismount(true) {}

    VehicleSeatEntry const* seatInfo;
    ObjectGuid              passengerGuid;
    bool IsProtectPassenger() const;
    bool b_dismount:1;
};

typedef std::map<SeatId, VehicleSeat> SeatMap;

enum AccessoryFlags
{
    ACCESSORY_FLAG_NONE   = 0x00000000,
    ACCESSORY_FLAG_MINION = 0x00000001,
    ACCESSORY_FLAG_HIDE   = 0x00000002
};

struct VehicleAccessory
{
    uint32   vehicleEntry;
    int32    seatId;
    uint32   passengerEntry;
    uint32   m_flags;
    float    m_offsetX;
    float    m_offsetY;
    float    m_offsetZ;
    float    m_offsetO;

    void Offset(float x, float y, float z, float o = 0.0f) { m_offsetX = x; m_offsetY = y; m_offsetZ = z; m_offsetO = o; }
};

class MANGOS_DLL_SPEC VehicleKit : public TransportBase
{
    public:
        explicit VehicleKit(Unit* base, VehicleEntry const* entry);
        ~VehicleKit();

        VehicleEntry const* GetEntry() const { return m_vehicleEntry; }

        void Initialize(uint32 creatureEntry = 0);                                  ///< Initializes the accessories
        bool IsInitialized() const { return m_isInitialized; }

        void Reset();
        void InstallAccessory(SeatId seatID);

        bool HasEmptySeat(SeatId seatId) const;
        SeatId GetNextEmptySeatWithFlag(SeatId seatId, bool next = true, uint32 VehicleSeatFlag = 0) const;
        Unit* GetPassenger(SeatId seatId) const;
        bool AddPassenger(Unit* passenger, SeatId seatId = -1);
        void RemovePassenger(Unit* passenger, bool dismount = false);
        void RemoveAllPassengers();
        VehicleSeatEntry const* GetSeatInfo(Unit* passenger);
        VehicleSeatEntry const* GetSeatInfo(ObjectGuid const& guid);
        SeatId GetSeatId(Unit* passenger);
        SeatId GetSeatId(ObjectGuid const& guid);
        void SetDestination(float x, float y, float z, float o, float speed, float elevation);
        void SetDestination() { m_dst_x = 0.0f; m_dst_y = 0.0f; m_dst_z = 0.0f; m_dst_o = 0.0f; m_dst_speed = 0.0f; m_dst_elevation = 0.0f; m_dstSet = false; }
        void DisableDismount(Unit* passenger);

        Unit* GetBase() const { return (Unit*)GetOwner(); }
        Aura* GetControlAura(Unit* passenger);

    private:
        // Internal use to calculate the boarding position
        virtual Position CalculateBoardingPositionOf(Position const& pos) const override;

        Position CalculateSeatPositionOf(VehicleSeatEntry const* seatInfo) const;

        void UpdateFreeSeatCount();
        void InstallAccessory(VehicleAccessory const* accessory);
        void InstallAllAccessories(uint32 entry);

        void Dismount(Unit* passenger, VehicleSeatEntry const* pSeatInfo = nullptr);
        void DismountFromFlyingVehicle(Unit* passenger);

        VehicleEntry const* m_vehicleEntry;
        SeatMap m_Seats;
        uint32  m_uiNumFreeSeats;
        float   m_dst_x, m_dst_y, m_dst_z, m_dst_o, m_dst_speed, m_dst_elevation;
        bool    m_isInitialized:1;                               // Internal use to store if the accessory is initialized
        bool    m_dstSet:1;
};

#endif
