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

#ifndef MANGOS_MAP_H
#define MANGOS_MAP_H

#include "Common.h"
#include "Platform/Define.h"
#include "Policies/ThreadingModel.h"

#include "DBCStructure.h"
#include "GridDefines.h"
#include "Cell.h"
#include "Object.h"
#include "ObjectGuid.h"
#include "Timer.h"
#include "SharedDefines.h"
#include "GridMap.h"
#include "GameSystem/GridRefManager.h"
#include "MapRefManager.h"
#include "Utilities/TypeList.h"
#include "ScriptMgr.h"
#include "CreatureLinkingMgr.h"
#include "vmap/DynamicTree.h"
#include "WorldObjectEvents.h"
#include <boost/thread/lock_guard.hpp>
#include <boost/thread/recursive_mutex.hpp>

#include <bitset>
#include <list>

struct CreatureInfo;
class Creature;
class Unit;
class WorldPacket;
class InstanceData;
class Group;
class Transport;
class MapPersistentState;
class WorldPersistentState;
class DungeonPersistentState;
class BattleGroundPersistentState;
struct ScriptInfo;
class BattleGround;
class GridMap;
class GameObjectModel;
class TerrainInfo;
class WeatherSystem;

// GCC have alternative #pragma pack(N) syntax and old gcc version not support pack(push,N), also any gcc version not support it at some platform
#if defined( __GNUC__ )
#pragma pack(1)
#else
#pragma pack(push,1)
#endif

struct InstanceTemplate
{
    uint32 map;                                             // instance map
    uint32 parent;                                          // non-continent parent instance (for instance with entrance in another instances)
                                                            // or 0 (not related to continent 0 map id)
    uint32 levelMin;
    uint32 levelMax;
    uint32 script_id;
};

struct WorldTemplate
{
    uint32 map;                                             // non-instance map
    uint32 script_id;
};

enum LevelRequirementVsMode
{
    LEVELREQUIREMENT_HEROIC = 70
};

#if defined( __GNUC__ )
#pragma pack()
#else
#pragma pack(pop)
#endif

#define MIN_UNLOAD_DELAY      1                             // immediate unload

struct MapID
{
    explicit MapID(uint32 id) : nMapId(id), nInstanceId(0) {}
    MapID(uint32 id, uint32 instid) : nMapId(id), nInstanceId(instid) {}

    bool operator<(const MapID& val) const
    {
        if (nMapId == val.nMapId)
            return nInstanceId < val.nInstanceId;

        if (IsContinent() && !val.IsContinent())
            return true;
        else if (!IsContinent() && val.IsContinent())
            return false;

        return nMapId < val.nMapId;
    }

    bool operator==(const MapID& val) const { return nMapId == val.nMapId && nInstanceId == val.nInstanceId; }

    bool IsContinent() const
    {
        return nMapId == 0 || nMapId == 1 || nMapId == 530 || nMapId == 571;
    };

    uint32 const& GetId() const { return nMapId; };
    uint32 const& GetInstanceId() const { return nInstanceId; };

    uint32 nMapId;
    uint32 nInstanceId;
};

HASH_NAMESPACE_START
template<> class hash <MapID>
{
    public: size_t operator()(const MapID& __x) const { return (size_t)((__x.GetId() << 16) | (__x.GetInstanceId())); }
};
HASH_NAMESPACE_END

typedef UNORDERED_SET<MapID> MapIDSet;

struct ZoneID : public MapID
{
    explicit ZoneID(uint32 mapId, uint32 zoneId) : MapID(mapId), nZoneId(zoneId) {}
    ZoneID(uint32 mapId, uint32 instid, uint32 zoneId) : MapID(mapId, instid), nZoneId(zoneId) {}
    bool operator == (const ZoneID& val) const { return GetId() == val.GetId() && GetInstanceId() == val.GetInstanceId() && GetZoneId() == val.GetZoneId(); }
    uint32 const& GetZoneId() const { return nZoneId; };
    uint32 nZoneId;
};

HASH_NAMESPACE_START
template<> class hash <ZoneID>
{
    public: size_t operator()(const ZoneID& __x) const { return (size_t)((__x.GetId() << 24) | (__x.GetInstanceId() << 16) | (__x.GetZoneId())) ; }
};
HASH_NAMESPACE_END

typedef UNORDERED_MAP<ObjectGuid,GuidSet>  AttackersMap;

struct LoadingObjectQueueMember
{
    explicit LoadingObjectQueueMember(uint32 _guid, TypeID _objectTypeID, GridType& _grid) :
        guid(_guid), objectTypeID(_objectTypeID), grid(_grid)
    {}
    uint32 guid;
    TypeID objectTypeID;
    GridType& grid;
};

class LoadingObjectsCompare
{
    public:
        LoadingObjectsCompare()
        {}

        bool operator() (LoadingObjectQueueMember const* lqm, LoadingObjectQueueMember const* rqm) const
        {
            return lqm->objectTypeID < rqm->objectTypeID;
        };
};

typedef std::priority_queue<LoadingObjectQueueMember*, std::vector<LoadingObjectQueueMember*>, LoadingObjectsCompare> LoadingObjectsQueue;

class MANGOS_DLL_SPEC Map : public GridRefManager<NGridType>
{
    friend class MapReference;
    friend class ObjectGridLoader;
    friend class ObjectWorldLoader;

    protected:
        Map(uint32 id, time_t, uint32 InstanceId, uint8 SpawnMode);

    public:
        virtual ~Map();

        MapEntry const* GetEntry() const { return i_mapEntry; }

        // currently unused for normal maps
        bool CanUnload(uint32 diff)
        {
            if(!m_unloadTimer)
                return false;
            if(m_unloadTimer <= diff)
                return true;
            m_unloadTimer -= diff;
            return false;
        }

        virtual bool Add(Player *);
        virtual void Remove(Player *, bool);
        template<class T> void Add(T *);
        template<class T> void Remove(T *, bool);

        void DeleteFromWorld(Player* player);                   // player object will deleted at call

        virtual void Update(const uint32&);

        void MessageBroadcast(Player const*, WorldPacket*, bool to_self);
        void MessageBroadcast(WorldObject const*, WorldPacket*);
        void MessageDistBroadcast(Player const*, WorldPacket*, float dist, bool to_self, bool own_team_only = false);
        void MessageDistBroadcast(WorldObject const*, WorldPacket*, float dist);

        float GetVisibilityDistance(WorldObject const* obj = nullptr) const;
        //function for setting up visibility distance for maps on per-type/per-Id basis
        virtual void InitVisibilityDistance();

        // Half-hack method for use with visible-over-grid active objects (like big WB and MOTransport)
        bool IsVisibleGlobally(ObjectGuid const& guid);

        template<class T> void Relocation(T* object, Position const& pos);

        // FIXME - remove this wrapper after SD2 correct
        void CreatureRelocation(Creature* object, float x, float y, float z, float orientation);

        template<class T, class CONTAINER> void Visit(const Cell& cell, TypeContainerVisitor<T, CONTAINER> &visitor);

        bool IsRemovalGrid(float x, float y) const
        {
            GridPair p = MaNGOS::ComputeGridPair(x, y);
            return (!getNGrid(p.x_coord, p.y_coord) || getNGrid(p.x_coord, p.y_coord)->GetGridState() == GRID_STATE_REMOVAL);
        }

        bool IsLoaded(float x, float y) const
        {
            GridPair p = MaNGOS::ComputeGridPair(x, y);
            return loaded(p);
        }
        bool PreloadGrid(float x, float y);
        void ActivateGrid(WorldLocation const& loc);
        void ActivateGrid(NGridType* nGrid);

        bool GetUnloadLock(const GridPair &p) const { return getNGrid(p.x_coord, p.y_coord)->getUnloadLock(); }
        void SetUnloadLock(const GridPair &p, bool on) { getNGrid(p.x_coord, p.y_coord)->setUnloadExplicitLock(on); }
        void LoadGrid(const Cell& cell, bool no_unload = false);
        bool UnloadGrid(NGridType& grid, bool pForce);
        bool UpdateGridState(NGridType& grid, GridInfo& gridInfo, uint32 const& t_diff);

        virtual void UnloadAll(bool pForce);

        void ResetGridExpiry(NGridType& grid, float factor = 1.0f) const
        {
            grid.ResetTimeTracker((time_t)((float)GetGridExpiry() * factor));
        }

        time_t GetGridExpiry() const;
        uint32 GetId(void) const { return i_id; }

        // some calls like isInWater should not use vmaps due to processor power
        // can return INVALID_HEIGHT if under z+2 z coord not found height

        virtual void RemoveAllObjectsInRemoveList();

        bool CreatureRespawnRelocation(Creature *c);        // used only in CreatureRelocation and ObjectGridUnloader

        bool CheckGridIntegrity(Creature* c, bool moved) const;

        uint32 GetInstanceId() const { return i_InstanceId; }
        virtual bool CanEnter(Player* /*player*/) { return true; }
        const char* GetMapName() const;

        // have meaning only for instanced map (that have set real difficulty), NOT USE its for BaseMap
        // _currently_ spawnmode == difficulty, but this can be changes later, so use appropriate spawmmode/difficult functions
        // for simplify later code support
        // regular difficulty = continent/dungeon normal/first raid normal difficulty
        uint8 GetSpawnMode() const { return (i_spawnMode); }
        Difficulty GetDifficulty() const { return Difficulty(GetSpawnMode()); }
        bool IsRegularDifficulty() const { return GetDifficulty() == REGULAR_DIFFICULTY; }
        uint32 GetMaxPlayers() const;                       // dependent from map difficulty
        uint32 GetMaxResetDelay() const;                    // dependent from map difficulty
        MapDifficultyEntry const* GetMapDifficulty() const; // dependent from map difficulty

        bool Instanceable() const { return i_mapEntry && i_mapEntry->Instanceable(); }
        // NOTE: this duplicate of Instanceable(), but Instanceable() can be changed when BG also will be instanceable
        bool IsDungeon() const { return i_mapEntry && i_mapEntry->IsDungeon(); }
        bool IsRaid() const { return i_mapEntry && i_mapEntry->IsRaid(); }
        bool IsRaidOrHeroicDungeon() const { return IsRaid() || GetDifficulty() > DUNGEON_DIFFICULTY_NORMAL; }
        bool IsHeroic() const { return IsRaid() ? GetDifficulty() >= RAID_DIFFICULTY_10MAN_HEROIC : GetDifficulty() >= DUNGEON_DIFFICULTY_HEROIC; }
        bool IsBattleGround() const { return i_mapEntry && i_mapEntry->IsBattleGround(); }
        bool IsBattleArena() const { return i_mapEntry && i_mapEntry->IsBattleArena(); }
        bool IsBattleGroundOrArena() const { return i_mapEntry && i_mapEntry->IsBattleGroundOrArena(); }
        bool IsContinent() const { return i_mapEntry && i_mapEntry->IsContinent(); }

        // can't be nullptr for loaded map
        MapPersistentState* GetPersistentState() const;

        void AddObjectToRemoveList(WorldObject *obj, bool immediateCleanup = false);
        void RemoveObjectFromRemoveList(WorldObject* obj);

        void UpdateObjectVisibility(WorldObject* obj, Cell cell, CellPair cellpair);

        void resetMarkedCells() { marked_cells.reset(); }
        bool isCellMarked(uint32 pCellId) { return marked_cells.test(pCellId); }
        void markCell(uint32 pCellId) { marked_cells.set(pCellId); }

        bool HavePlayers() const { return !m_mapRefManager.isEmpty(); }
        bool isFull() const { return GetPlayersCountExceptGMs() >= GetMaxPlayers(); }
        uint32 GetPlayersCountExceptGMs() const;
        bool ActiveObjectsNearGrid(uint32 x,uint32 y) const;

        /// Send a Packet to all players on a map
        void SendToPlayers(WorldPacket const* data) const;
        /// Send a Packet to all players in a zone. Return false if no player found
        bool SendToPlayersInZone(WorldPacket const* data, uint32 zoneId) const;

        typedef MapRefManager PlayerList;
        PlayerList const& GetPlayers() const { return m_mapRefManager; }

        // per-map script storage
        enum ScriptExecutionParam
        {
            SCRIPT_EXEC_PARAM_NONE                    = 0x00,   // Start regardless if already started
            SCRIPT_EXEC_PARAM_UNIQUE_BY_SOURCE        = 0x01,   // Start Script only if not yet started (uniqueness identified by id and source)
            SCRIPT_EXEC_PARAM_UNIQUE_BY_TARGET        = 0x02,   // Start Script only if not yet started (uniqueness identified by id and target)
            SCRIPT_EXEC_PARAM_UNIQUE_BY_SOURCE_TARGET = 0x03,   // Start Script only if not yet started (uniqueness identified by id, source and target)
        };
        bool ScriptsStart(ScriptMapMapName const& scripts, uint32 id, Object* source, Object* target, ScriptExecutionParam execParams = SCRIPT_EXEC_PARAM_NONE);
        void ScriptCommandStart(ScriptInfo const& script, uint32 delay, Object* source, Object* target);
        ScriptAction* GetNextSheduledScript();
        bool EraseScriptAction(ScriptAction* action);

        // must called with AddToWorld
        void AddToActive(WorldObject* obj);
        // must called with RemoveFromWorld
        void RemoveFromActive(WorldObject* obj);
        GuidQueue GetActiveObjects();

        Player* GetPlayer(ObjectGuid guid, bool globalSearch = false);
        Creature* GetCreature(ObjectGuid  guid);
        Pet* GetPet(ObjectGuid guid);
        Creature* GetAnyTypeCreature(ObjectGuid guid);      // normal creature or pet or vehicle
        GameObject* GetGameObject(ObjectGuid guid);
        DynamicObject* GetDynamicObject(ObjectGuid guid);
        Transport* GetTransport(ObjectGuid guid);
        Corpse* GetCorpse(ObjectGuid guid);                 // !!! find corpse can be not in world
        Unit* GetUnit(ObjectGuid guid);                     // only use if sure that need objects at current map, specially for player case
        WorldObject* GetWorldObject(ObjectGuid guid);       // only use if sure that need objects at current map, specially for player case

        // Container maked without any locks (for faster search), need make external locks!
        typedef UNORDERED_MAP<ObjectGuid, WorldObject*> MapStoredObjectTypesContainer;
        MapStoredObjectTypesContainer GetObjectsStore() { return m_objectsStore; }
        void InsertObject(WorldObject* object);
        void EraseObject(WorldObject* object);
        void EraseObject(ObjectGuid guid);
        WorldObject* FindObject(ObjectGuid guid);

        // Manipulation with objects update queue
        void AddUpdateObject(ObjectGuid guid);
        void RemoveUpdateObject(ObjectGuid guid);
        GuidSet const* GetObjectsUpdateQueue() { return &i_objectsToClientUpdate; };
        ObjectGuid GetNextObjectFromUpdateQueue();

        // DynObjects currently
        uint32 GenerateLocalLowGuid(HighGuid guidhigh);

        //get corresponding TerrainData object for this particular map
        const TerrainInfo * GetTerrain() const { return m_TerrainData; }

        void CreateInstanceData(bool load);
        InstanceData* GetInstanceData() const { return i_data; }
        uint32 GetScriptId() const { return i_script_id; }

        void MonsterYellToMap(ObjectGuid guid, int32 textId, Language language, Unit const* target) const;
        void MonsterYellToMap(CreatureInfo const* cinfo, int32 textId, Language language, Unit const* target, uint32 senderLowGuid = 0) const;
        void PlayDirectSoundToMap(uint32 soundId, uint32 zoneId = 0) const;

        // WeatherSystem
        WeatherSystem* GetWeatherSystem() const { return m_weatherSystem; }
        /** Set the weather in a zone on this map
        * @param zoneId set the weather for which zone
        * @param type What weather to set
        * @param grade how strong the weather should be
        * @param permanently set the weather permanently?
        */
        void SetWeather(uint32 zoneId, WeatherType type, float grade, bool permanently);

        // WorldState operations
        void UpdateWorldState(uint32 state, uint32 value);

        // Attacker per-map storage operations
        void AddAttackerFor(ObjectGuid const& targetGuid, ObjectGuid const& attackerGuid);
        void RemoveAttackerFor(ObjectGuid const& targetGuid, ObjectGuid const& attackerGuid);
        void RemoveAllAttackersFor(ObjectGuid const& targetGuid);
        GuidSet& GetAttackersFor(ObjectGuid const& targetGuid);
        void CreateAttackersStorageFor(ObjectGuid const& targetGuid);
        void RemoveAttackersStorageFor(ObjectGuid const& targetGuid);
        bool IsInCombat(ObjectGuid const& targetGuid) const;

        // Get Holder for Creature Linking
        CreatureLinkingHolder* GetCreatureLinkingHolder() { return &m_creatureLinkingHolder; }

        // map restarting system
        bool IsBroken() { return m_broken; };
        void SetBroken( bool _value = true ) { m_broken = _value; };
        void ForcedUnload();

        // Dynamic VMaps
        DynamicMapTree const& GetDynamicMapTree() const;
        float GetHeight(uint32 phasemask, float x, float y, float z) const;
        bool GetHeightInRange(uint32 phasemask, float x, float y, float &z, float maxSearchDist = 4.0f) const;
        bool IsInLineOfSight(float x1, float y1, float z1, float x2, float y2, float z2, uint32 phasemask) const;
        bool GetHitPosition(float srcX, float srcY, float srcZ, float& destX, float& destY, float& destZ, uint32 phasemask, float modifyDist) const;
        void DynamicMapTreeBalance();
        void DynamicMapTreeUpdate(uint32 const& t_diff);
        bool IsInLineOfSightByDynamicMapTree(float srcX, float srcY, float srcZ, float destX, float destY, float destZ, uint32 phasemask) const;

        void InsertGameObjectModel(const GameObjectModel& mdl);
        void RemoveGameObjectModel(const GameObjectModel& mdl);
        bool ContainsGameObjectModel(const GameObjectModel& mdl) const;

        void AddLoadingObject(LoadingObjectQueueMember* obj);
        LoadingObjectQueueMember* GetNextLoadingObject();
        LoadingObjectsQueue const& GetLoadingObjectsQueue() { return i_loadingObjectQueue; };
        bool IsLoadingObjectsQueueEmpty() const { return i_loadingObjectQueue.empty(); };

        // Event handler
        WorldObjectEventProcessor* GetEvents();
        void UpdateEvents(uint32 update_diff);
        void KillAllEvents(bool force);
        void AddEvent(BasicEvent* Event, uint64 e_time, bool set_addtime = true);

        // Random on map generation
        bool GetReachableRandomPosition(Unit* unit, float& x, float& y, float& z, float radius);
        bool GetReachableRandomPointOnGround(uint32 phaseMask, float& x, float& y, float& z, float radius);
        bool GetRandomPointInTheAir(uint32 phaseMask, float& x, float& y, float& z, float radius);
        bool GetRandomPointUnderWater(uint32 phaseMask, float& x, float& y, float& z, float radius, float water_z);

    private:
        void LoadMapAndVMap(int gx, int gy);

        void SendInitSelf( Player * player );

        void SendInitActiveObjects(Player* player);
        void SendRemoveActiveObjects(Player* player);

        void SendRemoveNotifyToStoredClients(WorldObject* object, bool destroy = false);

        bool CreatureCellRelocation(Creature* creature, Cell new_cell);

        bool loaded(GridPair const& p) const;
        void EnsureGridCreated(GridPair const& p);
        bool EnsureGridLoaded(Cell const& c);
        void EnsureGridLoadedAtEnter(Cell const& c, Player* player = nullptr);

        void buildNGridLinkage(NGridType* pNGridType) { pNGridType->link(this); }

        template<class T> void AddType(T *obj);
        template<class T> void RemoveType(T *obj, bool);

        NGridType* getNGrid(uint32 x, uint32 y) const
        {
            MANGOS_ASSERT(x < MAX_NUMBER_OF_GRIDS);
            MANGOS_ASSERT(y < MAX_NUMBER_OF_GRIDS);
            return i_grids[x][y];
        }

        NGridType const* getNGridWithoutLock(uint32 x, uint32 y) const
        {
            MANGOS_ASSERT(x < MAX_NUMBER_OF_GRIDS);
            MANGOS_ASSERT(y < MAX_NUMBER_OF_GRIDS);
            return i_grids[x][y];
        }

        template<class T> void LoadObjectToGrid(uint32& guid, GridType& grid, BattleGround* bg);
        template<class T> void setUnitCell(T* /*obj*/) {}
        void setUnitCell(Creature* obj);

        bool IsGridObjectDataLoaded(NGridType const* grid) const;
        void SetGridObjectDataLoaded(bool pLoaded, NGridType& grid);

        void setNGrid(NGridType* grid, uint32 x, uint32 y);
        void ScriptsProcess();

        void SendObjectUpdates();

        GuidSet i_objectsToClientUpdate;

        LoadingObjectsQueue i_loadingObjectQueue;

    protected:
        MapEntry const* i_mapEntry;
        uint8 i_spawnMode;
        uint32 i_id;
        uint32 i_InstanceId;
        uint32 m_unloadTimer;
        float m_VisibleDistance;

        MapRefManager m_mapRefManager;

        GuidSet m_activeObjects;
        GuidSet m_activeObjectsSafeCopy;
        MapStoredObjectTypesContainer m_objectsStore;

    private:

        NGridType* i_grids[MAX_NUMBER_OF_GRIDS][MAX_NUMBER_OF_GRIDS];

        //Shared geodata object with map coord info...
        TerrainInfo* const m_TerrainData;
        DynamicMapTree m_dyn_tree;

        bool m_bLoadedGrids[MAX_NUMBER_OF_GRIDS][MAX_NUMBER_OF_GRIDS];

        std::bitset<TOTAL_NUMBER_OF_CELLS_PER_MAP*TOTAL_NUMBER_OF_CELLS_PER_MAP> marked_cells;

        UNORDERED_SET<WorldObject*> i_objectsToRemove;

        typedef std::multimap<time_t, ScriptAction> ScriptScheduleMap;
        ScriptScheduleMap m_scriptSchedule;

        InstanceData* i_data;
        uint32 i_script_id;

        // Map local low guid counters
        ObjectGuidGenerator<HIGHGUID_UNIT> m_CreatureGuids;
        ObjectGuidGenerator<HIGHGUID_GAMEOBJECT> m_GameObjectGuids;
        ObjectGuidGenerator<HIGHGUID_DYNAMICOBJECT> m_DynObjectGuids;
        ObjectGuidGenerator<HIGHGUID_PET> m_PetGuids;

        // Type specific code for add/remove to/from grid
        template<class T>
            void AddToGrid(T*, NGridType *, Cell const&);

        template<class T>
            void RemoveFromGrid(T*, NGridType *, Cell const&);

        // Holder for information about linked mobs
        CreatureLinkingHolder m_creatureLinkingHolder;

        AttackersMap        m_attackersMap;
        bool                m_broken;
        // WeatherSystem
        WeatherSystem* m_weatherSystem;
        //EventProcessor
        WorldObjectEventProcessor m_Events;
};

class MANGOS_DLL_SPEC WorldMap : public Map
{
    private:
        using Map::GetPersistentState;                      // hide in subclass for overwrite
    public:
        WorldMap(uint32 id, time_t expiry) : Map(id, expiry, 0, REGULAR_DIFFICULTY) {}
        virtual ~WorldMap() {}

        // can't be nullptr for loaded map
        WorldPersistentState* GetPersistanceState() const;
};

class MANGOS_DLL_SPEC DungeonMap : public Map
{
    private:
        using Map::GetPersistentState;                      // hide in subclass for overwrite
    public:
        DungeonMap(uint32 id, time_t, uint32 InstanceId, uint8 SpawnMode);
        virtual ~DungeonMap();
        bool Add(Player *);
        void Remove(Player *, bool);
        void Update(const uint32&);
        bool Reset(InstanceResetMethod method);
        void PermBindAllPlayers(Player *player, bool permanent = true);
        void UnloadAll(bool pForce);
        bool CanEnter(Player* player);
        void SendResetWarnings(uint32 timeLeft) const;
        void SetResetSchedule(bool on);

        // can't be nullptr for loaded map
        DungeonPersistentState* GetPersistanceState() const;

        virtual void InitVisibilityDistance();
    private:
        bool m_resetAfterUnload;
        bool m_unloadWhenEmpty;
};

class MANGOS_DLL_SPEC BattleGroundMap : public Map
{
    private:
        using Map::GetPersistentState;                      // hide in subclass for overwrite
    public:
        BattleGroundMap(uint32 id, time_t, uint32 InstanceId, uint8 spawnMode);
        virtual ~BattleGroundMap();

        void Update(const uint32&);
        bool Add(Player *);
        void Remove(Player *, bool);
        bool CanEnter(Player* player);
        void SetUnload();
        void UnloadAll(bool pForce);

        virtual void InitVisibilityDistance();
        BattleGround* GetBG() { return m_bg; }
        void SetBG(BattleGround* bg) { m_bg = bg; }

        // can't be nullptr for loaded map
        BattleGroundPersistentState* GetPersistanceState() const;

    private:
        BattleGround* m_bg;
};

template<class T, class CONTAINER>
inline void Map::Visit(Cell const& cell, TypeContainerVisitor<T, CONTAINER>& visitor)
{
    NGridType const* cgrid = getNGridWithoutLock(cell.GridX(), cell.GridY());
    if (cell.NoCreate() && !IsGridObjectDataLoaded(cgrid))
        return;
    if (!IsGridObjectDataLoaded(cgrid))
        EnsureGridLoaded(cell);
    NGridType& grid = *getNGrid(cell.GridX(), cell.GridY());
    grid.Visit(cell.CellX(), cell.CellY(), visitor);
}

#endif
