/*
 * Copyright (C) 2005-2012 MaNGOS <http://getmangos.com/>
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

#ifndef MANGOS_MAPMANAGER_H
#define MANGOS_MAPMANAGER_H

#include "Common.h"
#include "Platform/Define.h"
#include "Policies/Singleton.h"
#include "Map.h"

class BattleGround;

class MapManager : public MaNGOS::Singleton<MapManager, MaNGOS::ClassLevelLockable<MapManager, std::recursive_mutex> >
{
    friend class MaNGOS::OperatorNew<MapManager>;

	typedef std::recursive_mutex LOCK_TYPE;
	typedef std::lock_guard<LOCK_TYPE> LOCK_TYPE_GUARD;
	typedef MaNGOS::ClassLevelLockable<MapManager, std::recursive_mutex>::Lock Guard;

    public:
        typedef UNORDERED_MAP<MapID, Map*> MapMapType;

        Map* CreateMap(uint32, WorldObject const* obj);
        Map* CreateBgMap(uint32 mapid, BattleGround* bg);
        Map* FindMap(uint32 mapid, uint32 instanceId = 0) const;
        Map* FindFirstMap(uint32 mapid) const;
        Map* GetMapPtr(uint32 mapid, uint32 instanceId = 0);

        // only const version for outer users
        void DeleteInstance(uint32 mapid, uint32 instanceId);

        void Initialize(void);
        void Update(uint32);

        void SetMapUpdateInterval(uint32 t)
        {
            if (t < MIN_MAP_UPDATE_DELAY)
                t = MIN_MAP_UPDATE_DELAY;

            m_timer.SetInterval(t);
            m_timer.Reset();
        }

        //void LoadGrid(int mapid, int instId, float x, float y, const WorldObject* obj, bool no_unload = false);
        void UnloadAll();

        static bool ExistMapAndVMap(uint32 mapid, float x, float y);
        static bool IsValidMAP(uint32 mapid);
        static bool IsTransportMap(uint32 mapid);

        static bool IsValidMapCoord(uint32 mapid, float x,float y)
        {
            return IsValidMAP(mapid) && MaNGOS::IsValidMapCoord(x,y);
        }

        static bool IsValidMapCoord(uint32 mapid, float x,float y,float z)
        {
            return IsValidMAP(mapid) && MaNGOS::IsValidMapCoord(x,y,z);
        }

        static bool IsValidMapCoord(uint32 mapid, float x,float y,float z,float o)
        {
            return IsValidMAP(mapid) && MaNGOS::IsValidMapCoord(x,y,z,o);
        }

        static bool IsValidMapCoord(WorldLocation const& loc)
        {
            return IsValidMapCoord(loc.GetMapId(),loc.x,loc.y,loc.z,loc.orientation);
        }

        // modulos a radian orientation to the range of 0..2PI
        static float NormalizeOrientation(float o)
        {
            // fmod only supports positive numbers. Thus we have
            // to emulate negative numbers
            if (o < 0)
            {
                float mod = o *-1;
                mod = fmod(mod, 2.0f*M_PI_F);
                mod = -mod+2.0f*M_PI_F;
                return mod;
            }
            return fmod(o, 2.0f*M_PI_F);
        }

        void RemoveAllObjectsInRemoveList();

        bool CanPlayerEnter(uint32 mapid, Player* player);
        void InitializeVisibilityDistanceInfo();

        /* statistics */
        uint32 GetNumInstances();
        std::string GetStrMaps();
        uint32 GetNumPlayersInInstances();

        //get list of all maps
        const MapMapType& Maps() const { return m_maps; }

        template<typename Do>
        void DoForAllMapsWithMapId(uint32 mapId, Do& _do);

        void UpdateLoadBalancer(bool b_start);

    private:

        MapManager();
        ~MapManager();

        MapManager(const MapManager &);
        MapManager& operator=(const MapManager &);

        Map* CreateInstance(uint32 id, Player * player);
        DungeonMap* CreateDungeonMap(uint32 id, uint32 InstanceId, Difficulty difficulty, DungeonPersistentState *save = nullptr);
        BattleGroundMap* CreateBattleGroundMap(uint32 id, uint32 InstanceId, BattleGround* bg);

        MapMapType         m_maps;
        ShortIntervalTimer m_timer;
};

template<typename Do>
inline void MapManager::DoForAllMapsWithMapId(uint32 mapId, Do& _do)
{
    for(MapMapType::const_iterator itr = m_maps.begin(); itr != m_maps.end(); ++itr)
    {
        if (itr->first.nMapId == mapId)
            _do(&*(itr->second));
    }
}

#define sMapMgr MapManager::Instance()

#endif
