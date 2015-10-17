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

#include "Map.h"
#include "MapManager.h"
#include "Player.h"
#include "GridNotifiers.h"
#include "Log.h"
#include "CellImpl.h"
#include "InstanceData.h"
#include "GridNotifiersImpl.h"
#include "Transports.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "World.h"
#include "ScriptMgr.h"
#include "Group.h"
#include "MapRefManager.h"
#include "DBCEnums.h"
#include "MapPersistentStateMgr.h"
#include "VMapFactory.h"
#include "MoveMap.h"
#include "MoveMapSharedDefines.h"
#include "BattleGround/BattleGroundMgr.h"
#include "Calendar.h"
#include "Chat.h"
#include "Weather.h"

Map::~Map()
{
    UnloadAll(true);

    if(!m_scriptSchedule.empty())
        sScriptMgr.DecreaseScheduledScriptCount(m_scriptSchedule.size());

    if (GetPersistentState())
        GetPersistentState()->SetUsedByMapState(nullptr);         // field pointer can be deleted after this

    if(i_data)
    {
        delete i_data;
        i_data = nullptr;
    }

    //sMapMgr.GetMapUpdater().MapStatisticDataRemove(this);

    // unload instance specific navigation data
    MMAP::MMapFactory::createOrGetMMapManager()->unloadMapInstance(m_TerrainData->GetMapId(), GetInstanceId());

    //release reference count
    if (m_TerrainData->Release())
        sTerrainMgr.UnloadTerrain(m_TerrainData->GetMapId());

    m_weatherSystem = nullptr;
    delete m_weatherSystem;    

    DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "Map::~Map removing map %u instance %u complete", GetId(), GetInstanceId());
}

void Map::LoadMapAndVMap(int gx,int gy)
{
    if (m_bLoadedGrids[gx][gy])
        return;

    m_bLoadedGrids[gx][gy] = true;

    if (m_TerrainData->Load(gx, gy))
        m_bLoadedGrids[gx][gy] = true;
}

Map::Map(uint32 id, time_t /*expiry*/, uint32 InstanceId, uint8 SpawnMode)
  : i_mapEntry (sMapStore.LookupEntry(id)), i_spawnMode(SpawnMode),
  i_id(id), i_InstanceId(InstanceId), m_unloadTimer(0),
  m_VisibleDistance(DEFAULT_VISIBILITY_DISTANCE),
  m_TerrainData(sTerrainMgr.LoadTerrain(id)),
  i_data(nullptr), i_script_id(0)
{
    m_CreatureGuids.Set(sObjectMgr.GetFirstTemporaryCreatureLowGuid());
    m_GameObjectGuids.Set(sObjectMgr.GetFirstTemporaryGameObjectLowGuid());

    for(unsigned int j=0; j < MAX_NUMBER_OF_GRIDS; ++j)
    {
        for(unsigned int idx=0; idx < MAX_NUMBER_OF_GRIDS; ++idx)
        {
            //z code
            m_bLoadedGrids[idx][j] = false;
            setNGrid(nullptr, idx, j);
        }
    }

    //lets initialize visibility distance for map
    Map::InitVisibilityDistance();

    //add reference for TerrainData object
    m_TerrainData->AddRef();

    MapPersistentState* persistentState = sMapPersistentStateMgr.AddPersistentState(i_mapEntry, GetInstanceId(), GetDifficulty(), 0, IsDungeon());
    persistentState->SetUsedByMapState(this);
    SetBroken(false);

    // start unload timer for non continent and BG/Arena maps
    if (!IsContinent() && !IsBattleGroundOrArena())
    {
        // the timer is started by default, and stopped when the first player joins
        // this make sure it gets unloaded if for some reason no player joins
        m_unloadTimer = sWorld.getConfig(CONFIG_UINT32_INSTANCE_UNLOAD_DELAY);
    }

    //if (GetInstanceId() && !sMapMgr.IsTransportMap(GetId()))
    //    sObjectMgr.LoadTransports(this);
    m_weatherSystem = new WeatherSystem(this);

    DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "Map::Map creating map %u instance %u complete", GetId(), GetInstanceId());
}

MapPersistentState* Map::GetPersistentState() const
{
    MapPersistentState* state = sMapPersistentStateMgr.GetPersistentState(GetId(), GetInstanceId());
    if (!state)
    {
        sLog.outError("Map::GetPersistentState requested, but map ( id %u, instance %u, difficulty %u ) not have this!", GetId(), GetInstanceId(), GetDifficulty());
        state = sMapPersistentStateMgr.AddPersistentState(i_mapEntry, GetInstanceId(), GetDifficulty(), 0, IsDungeon());
        state->SetUsedByMapState(const_cast<Map*>(this));
    }
    return state;
}

void Map::InitVisibilityDistance()
{
    //init visibility for continents
    m_VisibleDistance = World::GetMaxVisibleDistanceOnContinents();
}

// Template specialization of utility methods
template<class T>
void Map::AddToGrid(T* obj, NGridType *grid, Cell const& cell)
{
    (*grid)(cell.CellX(), cell.CellY()).template AddGridObject<T>(obj);
}

template<>
void Map::AddToGrid(Player* obj, NGridType *grid, Cell const& cell)
{
    (*grid)(cell.CellX(), cell.CellY()).AddWorldObject(obj);
}

template<>
void Map::AddToGrid(Corpse *obj, NGridType *grid, Cell const& cell)
{
    // add to world object registry in grid
    if(obj->GetType()!=CORPSE_BONES)
    {
        (*grid)(cell.CellX(), cell.CellY()).AddWorldObject(obj);
    }
    // add to grid object store
    else
    {
        (*grid)(cell.CellX(), cell.CellY()).AddGridObject(obj);
    }
}

template<>
void Map::AddToGrid(Creature* obj, NGridType *grid, Cell const& cell)
{
    // add to world object registry in grid
    if (obj->IsPet())
    {
        (*grid)(cell.CellX(), cell.CellY()).AddWorldObject<Creature>(obj);
        obj->SetCurrentCell(cell);
    }
    // add to grid object store
    else
    {
        (*grid)(cell.CellX(), cell.CellY()).AddGridObject<Creature>(obj);
        obj->SetCurrentCell(cell);
    }
}

template<>
void Map::AddToGrid(GameObject* obj, NGridType *grid, Cell const& cell)
{
    // add to world object registry in grid
    if (obj->GetObjectGuid().IsMOTransport())
    {
        (*grid)(cell.CellX(), cell.CellY()).AddWorldObject<GameObject>(obj);
    }
    // add to grid object store
    else
    {
        (*grid)(cell.CellX(), cell.CellY()).AddGridObject<GameObject>(obj);
    }
}

template<class T>
void Map::RemoveFromGrid(T* obj, NGridType *grid, Cell const& cell)
{
    (*grid)(cell.CellX(), cell.CellY()).template RemoveGridObject<T>(obj);
}

template<>
void Map::RemoveFromGrid(Player* obj, NGridType *grid, Cell const& cell)
{
    (*grid)(cell.CellX(), cell.CellY()).RemoveWorldObject(obj);
}

template<>
void Map::RemoveFromGrid(Corpse *obj, NGridType *grid, Cell const& cell)
{
    // remove from world object registry in grid
    if(obj->GetType()!=CORPSE_BONES)
    {
        (*grid)(cell.CellX(), cell.CellY()).RemoveWorldObject(obj);
    }
    // remove from grid object store
    else
    {
        (*grid)(cell.CellX(), cell.CellY()).RemoveGridObject(obj);
    }
}

template<>
void Map::RemoveFromGrid(Creature* obj, NGridType *grid, Cell const& cell)
{
    // remove from world object registry in grid
    if (obj->IsPet())
    {
        (*grid)(cell.CellX(), cell.CellY()).RemoveWorldObject<Creature>(obj);
    }
    // remove from grid object store
    else
    {
        (*grid)(cell.CellX(), cell.CellY()).RemoveGridObject<Creature>(obj);
    }
}

template<>
void Map::RemoveFromGrid(GameObject* obj, NGridType *grid, Cell const& cell)
{
    // remove from world object registry in grid
    if (obj->GetObjectGuid().IsMOTransport())
    {
        (*grid)(cell.CellX(), cell.CellY()).RemoveWorldObject<GameObject>(obj);
    }
    // remove from grid object store
    else
    {
        (*grid)(cell.CellX(), cell.CellY()).RemoveGridObject<GameObject>(obj);
    }
}

void Map::DeleteFromWorld(Player* pl)
{
    sObjectAccessor.RemoveObject(pl);
    delete pl;
}

void Map::setUnitCell(Creature* obj)
{
    CellPair xy_val = MaNGOS::ComputeCellPair(obj->GetPositionX(), obj->GetPositionY());
    Cell cell(xy_val);
    obj->SetCurrentCell(cell);
}

void
Map::EnsureGridCreated(const GridPair &p)
{
    if (!getNGridWithoutLock(p.x_coord, p.y_coord))
    {
        // Grid may be created while wait in semafore, requires double check
        if (getNGridWithoutLock(p.x_coord, p.y_coord))
            return;

        NGridType* grid = new NGridType(p.x_coord*MAX_NUMBER_OF_GRIDS + p.y_coord, p.x_coord, p.y_coord, sWorld.getConfig(CONFIG_UINT32_INTERVAL_GRIDCLEAN), sWorld.getConfig(CONFIG_BOOL_GRID_UNLOAD));
        setNGrid(grid, p.x_coord, p.y_coord);

        // build a linkage between this map and NGridType
        buildNGridLinkage(grid);
        grid->SetGridState(GRID_STATE_IDLE);
        ResetGridExpiry(*grid, 0.2f);

        //z coord
        int gx = (MAX_NUMBER_OF_GRIDS - 1) - p.x_coord;
        int gy = (MAX_NUMBER_OF_GRIDS - 1) - p.y_coord;

        if (!m_bLoadedGrids[gx][gy])
            LoadMapAndVMap(gx,gy);
    }
}

void
Map::EnsureGridLoadedAtEnter(Cell const& cell, Player* player)
{
    NGridType* grid = nullptr;

    if (EnsureGridLoaded(cell))
    {
        grid = getNGrid(cell.GridX(), cell.GridY());

        if (player)
        {
            DEBUG_FILTER_LOG(LOG_FILTER_PLAYER_MOVES, "Player %s enter cell[%u,%u] triggers of loading grid[%u,%u] on map %u", player->GetName(), cell.CellX(), cell.CellY(), cell.GridX(), cell.GridY(), i_id);
        }
        else
        {
            DEBUG_FILTER_LOG(LOG_FILTER_PLAYER_MOVES, "Active object nearby triggers of loading grid [%u,%u] on map %u", cell.GridX(), cell.GridY(), i_id);
        }

        ActivateGrid(grid);
    }
    else
        grid = getNGrid(cell.GridX(), cell.GridY());

    if (player)
        AddToGrid(player,grid,cell);
}

bool Map::EnsureGridLoaded(Cell const& cell)
{
    EnsureGridCreated(GridPair(cell.GridX(), cell.GridY()));
    NGridType const* cgrid = getNGridWithoutLock(cell.GridX(), cell.GridY());

    MANGOS_ASSERT(cgrid != nullptr);

    if (!IsGridObjectDataLoaded(cgrid))
    {
        NGridType* grid = const_cast<NGridType*>(cgrid);

        //it's important to set it loaded before loading!
        //otherwise there is a possibility of infinity chain (grid loading will be called many times for the same grid)
        //possible scenario:
        //active object A(loaded with loader.LoadN call and added to the  map)
        //summons some active object B, while B added to map grid loading called again and so on..
        SetGridObjectDataLoaded(true, *grid);
        ObjectGridLoader loader(*grid, this, cell);
        loader.LoadN();

        // Add resurrectable corpses to world object list in grid
        sObjectAccessor.AddCorpsesToGrid(GridPair(cell.GridX(),cell.GridY()),(*grid)(cell.CellX(), cell.CellY()), this);

        DynamicMapTreeBalance();
        return true;
    }

    return false;
}

bool Map::IsGridObjectDataLoaded(NGridType const* grid) const
{
    return grid ? grid->isGridObjectDataLoaded() : false;
}

void Map::SetGridObjectDataLoaded(bool pLoaded, NGridType& grid)
{
    grid.setGridObjectDataLoaded(pLoaded);
}

void Map::LoadGrid(const Cell& cell, bool no_unload)
{
    EnsureGridLoaded(cell);

    if(no_unload)
        getNGrid(cell.GridX(), cell.GridY())->setUnloadExplicitLock(true);
}

bool Map::PreloadGrid(float x, float y)
{
    CellPair pair = MaNGOS::ComputeCellPair(x, y);
    if(pair.x_coord >= TOTAL_NUMBER_OF_CELLS_PER_MAP || pair.y_coord >= TOTAL_NUMBER_OF_CELLS_PER_MAP )
    {
        sLog.outError("Map::PreloadGrid: invalid coordinates X:%f Y:%f grid cell [%u:%u]", x, y, pair.x_coord, pair.y_coord);
        return false;
    }
    Cell cell(pair);
    EnsureGridLoaded(cell);
    return IsLoadingObjectsQueueEmpty();
}

void Map::ActivateGrid(WorldLocation const& loc)
{
    Cell _cell(MaNGOS::ComputeCellPair(loc.x, loc.y));
    NGridType* nGrid = getNGrid(_cell.GridX(), _cell.GridY());
    ActivateGrid(nGrid);
}

void Map::ActivateGrid(NGridType* nGrid)
{
    if (nGrid)
    {
        ResetGridExpiry(*nGrid, 0.2f);
        if (nGrid->GetGridState() != GRID_STATE_ACTIVE)
            nGrid->SetGridState(GRID_STATE_ACTIVE);
    }
}

bool Map::Add(Player* player)
{
    if (!IsContinent() && !IsBattleGroundOrArena())
        m_unloadTimer = 0;

    if (FindObject(player->GetObjectGuid()))
    {
        sLog.outError("Map::Add map %u instance %u %s is found stored in Map object store, but trying to be addes!", GetId(), GetInstanceId(), player->GetObjectGuid() ? player->GetObjectGuid().GetString().c_str() : "<no GUID>");
    }

    player->GetMapRef().unlink();
    player->GetMapRef().link(this, player);
    player->SetMap(this);

    // update player state for other player and visa-versa
    AddToActive(player);
    CreateAttackersStorageFor(player->GetObjectGuid());

    CellPair p = MaNGOS::ComputeCellPair(player->GetPositionX(), player->GetPositionY());
    Cell cell(p);
    EnsureGridLoadedAtEnter(cell, player);
    NGridType* grid = getNGrid(cell.GridX(), cell.GridY());
    MANGOS_ASSERT(grid != nullptr);

    player->AddToWorld();
    SendInitSelf(player);
    SendInitActiveObjects(player);
    player->GetViewPoint().Event_AddedToWorld(&(*grid)(cell.CellX(), cell.CellY()));
    UpdateObjectVisibility(player,cell,p);

    DEBUG_FILTER_LOG(LOG_FILTER_PLAYER_MOVES, "Map::Add map %u instance %u add %s to grid [%u,%u]", GetId(), GetInstanceId(), player->GetObjectGuid().GetString().c_str(), cell.GridX(), cell.GridY());

    if (i_data)
        i_data->OnPlayerEnter(player);

    sLFGMgr.OnPlayerEnterMap(player, this);
    return true;
}

template<class T>
void
Map::Add(T* obj)
{
    MANGOS_ASSERT(obj);

    CellPair p = MaNGOS::ComputeCellPair(obj->GetPositionX(), obj->GetPositionY());
    if(p.x_coord >= TOTAL_NUMBER_OF_CELLS_PER_MAP || p.y_coord >= TOTAL_NUMBER_OF_CELLS_PER_MAP )
    {
        sLog.outError("Map::Add map %u instance %u %s have invalid coordinates X:%f Y:%f grid cell [%u:%u]", GetId(), GetInstanceId(), obj->GetObjectGuid() ? obj->GetObjectGuid().GetString().c_str() : "<no GUID>", obj->GetPositionX(), obj->GetPositionY(), p.x_coord, p.y_coord);
        return;
    }

    if (FindObject(obj->GetObjectGuid()))
    {
        if (obj->GetObjectGuid().IsCreatureOrVehicle())
        {
            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING,"Map::Add map %u instance %u %s is found stored in Map object store, but trying to be addes. Possible result of pool operations.", GetId(), GetInstanceId(), obj->GetObjectGuid() ? obj->GetObjectGuid().GetString().c_str() : "<no GUID>");
        }
        else
        {
            sLog.outError("Map::Add map %u instance %u %s is found stored in Map object store, but trying to be addes!", GetId(), GetInstanceId(), obj->GetObjectGuid() ? obj->GetObjectGuid().GetString().c_str() : "<no GUID>");
        }
    }

    obj->SetMap(this);
    if (obj->GetTypeId() == TYPEID_UNIT)
        CreateAttackersStorageFor(obj->GetObjectGuid());

    Cell cell(p);
    if(obj->isActiveObject())
        EnsureGridLoadedAtEnter(cell);
    else
        EnsureGridCreated(GridPair(cell.GridX(), cell.GridY()));

    NGridType* grid = getNGrid(cell.GridX(), cell.GridY());
    MANGOS_ASSERT(grid != nullptr);

    AddToGrid(obj,grid,cell);
    obj->AddToWorld();

    if(obj->isActiveObject())
        AddToActive(obj);

    DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "Map::Add map %u instance %u add %s to grid [%u,%u]", GetId(), GetInstanceId(), obj->GetObjectGuid() ? obj->GetObjectGuid().GetString().c_str() : "<no GUID>", cell.GridX(), cell.GridY());

    obj->GetViewPoint().Event_AddedToWorld(&(*grid)(cell.CellX(), cell.CellY()));
    UpdateObjectVisibility(obj,cell,p);
}

void Map::MessageBroadcast(Player const* player, WorldPacket* msg, bool to_self)
{
    CellPair p = MaNGOS::ComputeCellPair(player->GetPositionX(), player->GetPositionY());

    if(p.x_coord >= TOTAL_NUMBER_OF_CELLS_PER_MAP || p.y_coord >= TOTAL_NUMBER_OF_CELLS_PER_MAP )
    {
        sLog.outError("Map::MessageBroadcast: Player (GUID: %u) have invalid coordinates X:%f Y:%f grid cell [%u:%u]", player->GetGUIDLow(), player->GetPositionX(), player->GetPositionY(), p.x_coord, p.y_coord);
        return;
    }

    Cell cell(p);
    cell.SetNoCreate();

    EnsureGridCreated(GridPair(cell.GridX(), cell.GridY()));

    if (!loaded(GridPair(cell.data.Part.grid_x, cell.data.Part.grid_y)))
        return;

    MaNGOS::MessageDeliverer post_man(*player, msg, to_self);
    TypeContainerVisitor<MaNGOS::MessageDeliverer, WorldTypeMapContainer > message(post_man);
    cell.Visit(p, message, *this, *player, GetVisibilityDistance());
}

void Map::MessageBroadcast(WorldObject const* obj, WorldPacket* msg)
{
    CellPair p = MaNGOS::ComputeCellPair(obj->GetPositionX(), obj->GetPositionY());

    if(p.x_coord >= TOTAL_NUMBER_OF_CELLS_PER_MAP || p.y_coord >= TOTAL_NUMBER_OF_CELLS_PER_MAP )
    {
        sLog.outError("Map::MessageBroadcast: Object (GUID: %u TypeId: %u) have invalid coordinates X:%f Y:%f grid cell [%u:%u]", obj->GetGUIDLow(), obj->GetTypeId(), obj->GetPositionX(), obj->GetPositionY(), p.x_coord, p.y_coord);
        return;
    }

    Cell cell(p);
    cell.SetNoCreate();

    if (!loaded(GridPair(cell.data.Part.grid_x, cell.data.Part.grid_y)))
        return;

    //TODO: currently on continents when Visibility.Distance.InFlight > Visibility.Distance.Continents
    //we have alot of blinking mobs because monster move packet send is broken...
    MaNGOS::ObjectMessageDeliverer post_man(*obj,msg);
    TypeContainerVisitor<MaNGOS::ObjectMessageDeliverer, WorldTypeMapContainer > message(post_man);
    cell.Visit(p, message, *this, *obj, GetVisibilityDistance(obj));
}

void Map::MessageDistBroadcast(Player const* player, WorldPacket* msg, float dist, bool to_self, bool own_team_only)
{
    CellPair p = MaNGOS::ComputeCellPair(player->GetPositionX(), player->GetPositionY());

    if(p.x_coord >= TOTAL_NUMBER_OF_CELLS_PER_MAP || p.y_coord >= TOTAL_NUMBER_OF_CELLS_PER_MAP )
    {
        sLog.outError("Map::MessageBroadcast: Player (GUID: %u) have invalid coordinates X:%f Y:%f grid cell [%u:%u]", player->GetGUIDLow(), player->GetPositionX(), player->GetPositionY(), p.x_coord, p.y_coord);
        return;
    }

    Cell cell(p);
    cell.SetNoCreate();

    EnsureGridCreated(GridPair(cell.GridX(), cell.GridY()));

    if( !loaded(GridPair(cell.data.Part.grid_x, cell.data.Part.grid_y)) )
        return;

    MaNGOS::MessageDistDeliverer post_man(*player, msg, dist, to_self, own_team_only);
    TypeContainerVisitor<MaNGOS::MessageDistDeliverer , WorldTypeMapContainer > message(post_man);
    cell.Visit(p, message, *this, *player, dist);
}

void Map::MessageDistBroadcast(WorldObject const* obj, WorldPacket* msg, float dist)
{
    CellPair p = MaNGOS::ComputeCellPair(obj->GetPositionX(), obj->GetPositionY());

    if(p.x_coord >= TOTAL_NUMBER_OF_CELLS_PER_MAP || p.y_coord >= TOTAL_NUMBER_OF_CELLS_PER_MAP )
    {
        sLog.outError("Map::MessageBroadcast: Object (GUID: %u TypeId: %u) have invalid coordinates X:%f Y:%f grid cell [%u:%u]", obj->GetGUIDLow(), obj->GetTypeId(), obj->GetPositionX(), obj->GetPositionY(), p.x_coord, p.y_coord);
        return;
    }

    Cell cell(p);
    cell.SetNoCreate();

    if( !loaded(GridPair(cell.data.Part.grid_x, cell.data.Part.grid_y)) )
        return;

    MaNGOS::ObjectMessageDistDeliverer post_man(*obj, msg, dist);
    TypeContainerVisitor<MaNGOS::ObjectMessageDistDeliverer, WorldTypeMapContainer > message(post_man);
    cell.Visit(p, message, *this, *obj, dist);
}

bool Map::loaded(GridPair const& p) const
{
    NGridType const* grid = getNGridWithoutLock(p.x_coord, p.y_coord);
    return grid ? IsGridObjectDataLoaded(grid) : false;
}

void Map::Update(const uint32 &t_diff)
{
    DynamicMapTreeUpdate(t_diff);

    // Load all objects in begin of update diff (loading objects count limited by time)
    uint32 loadingObjectToGridUpdateTime = WorldTimer::getMSTime();
    BattleGround* bg = this->IsBattleGroundOrArena() ? ((BattleGroundMap*)this)->GetBG() : nullptr;
    while (WorldTimer::getMSTimeDiff(loadingObjectToGridUpdateTime, WorldTimer::getMSTime()) < sWorld.getConfig(CONFIG_UINT32_OBJECTLOADINGSPLITTER_ALLOWEDTIME)
        && !IsLoadingObjectsQueueEmpty())
    {
        LoadingObjectQueueMember* loadingObject = GetNextLoadingObject();
        if (!loadingObject)
            continue;

        switch(loadingObject->objectTypeID)
        {
            case TYPEID_UNIT:
            {
                LoadObjectToGrid<Creature>(loadingObject->guid, loadingObject->grid, bg);
                break;
            }
            case TYPEID_GAMEOBJECT:
            {
                LoadObjectToGrid<GameObject>(loadingObject->guid, loadingObject->grid, bg);
                break;
            }
            default:
                sLog.outError("loadingObject->guid = %u, loadingObject.objectTypeID = %u", loadingObject->guid, loadingObject->objectTypeID);
                break;
        }
        delete loadingObject;
    }

    UpdateEvents(t_diff);

    /// update worldsessions for existing players (stage 1)
    GuidQueue updateQueue = GetActiveObjects();
    while (!updateQueue.empty())
    {
        ObjectGuid guid = updateQueue.front();
        updateQueue.pop();

        if (!guid.IsPlayer())
            continue;

        Player* plr = GetPlayer(guid);

        if (plr && plr->IsInWorld())
        {
            if (WorldSession* pSession = plr->GetSession())
            {
                MapSessionFilter updater(pSession);
                pSession->Update(updater);
                // sending WorldState updates
                plr->SendUpdatedWorldStates(false);
            }
        }
    }

    /// update active objects (players also) at tick (stage 2)
    /*GuidQueue*/ updateQueue = GetActiveObjects();
    while (!updateQueue.empty())
    {
        ObjectGuid guid = updateQueue.front();
        updateQueue.pop();

        WorldObject* obj = GetWorldObject(guid);
        if (!obj || !obj->IsInWorld())
            continue;

        switch (guid.GetHigh())
        {
            case HIGHGUID_PLAYER:
            {
                Player* plr = dynamic_cast<Player*>(obj);
                if (plr)
                {
                    WorldObject::UpdateHelper helper(*plr);
                    helper.Update(t_diff);
                }
                break;
            }
            case HIGHGUID_MO_TRANSPORT:
            {
                // update active MO_TRANSPORT objects
                if (obj->isActiveObject() && obj->IsPositionValid())
                {
                    WorldObject::UpdateHelper helper(*obj);
                    helper.Update(t_diff);
                }
                break;
            }
            case HIGHGUID_TRANSPORT:
            // updating regular transports (after fully implementing Transport:: (NOT MOTransport!) class)
            default:
            // All another objects (active and not active) be updated in TypeContainerVisitor<> expressions
                break;
        }
    }

    /// update active cells around players and active objects
    resetMarkedCells();

    MaNGOS::ObjectUpdater updater(t_diff);
    // for creature
    TypeContainerVisitor<MaNGOS::ObjectUpdater, GridTypeMapContainer  > grid_object_update(updater);
    // for pets
    TypeContainerVisitor<MaNGOS::ObjectUpdater, WorldTypeMapContainer > world_object_update(updater);

    // player and non-player active objects (only cells re-mark)
    /*GuidQueue*/ updateQueue = GetActiveObjects();
    bool hasActive = !updateQueue.empty();
    while (!updateQueue.empty())
    {
        ObjectGuid guid = updateQueue.front();
        updateQueue.pop();

        WorldObject* obj = GetWorldObject(guid);

        if (!obj || !obj->IsInWorld() || !obj->isActiveObject() || !obj->IsPositionValid())
            continue;

        //lets update mobs/objects in ALL visible cells around player!
        CellArea area = Cell::CalculateCellArea(obj->GetPositionX(), obj->GetPositionY(), GetVisibilityDistance());
        for (uint32 x = area.low_bound.x_coord; x <= area.high_bound.x_coord; ++x)
        {
            for(uint32 y = area.low_bound.y_coord; y <= area.high_bound.y_coord; ++y)
            {
                // marked cells are those that have been visited
                // don't visit the same cell twice
                uint32 cell_id = (y * TOTAL_NUMBER_OF_CELLS_PER_MAP) + x;
                if(!isCellMarked(cell_id))
                {
                    markCell(cell_id);
                    CellPair pair(x,y);
                    Cell cell(pair);
                    cell.SetNoCreate();
                    Visit(cell, grid_object_update);
                    Visit(cell, world_object_update);
                }
            }
        }
    }

    // Send world objects and item update field changes
    if (hasActive)
        SendObjectUpdates();

    // Calculate and send map-related WorldState updates
    sWorldStateMgr.MapUpdate(this);

    // Don't unload grids if it's battleground, since we may have manually added GOs,creatures, those doesn't load from DB at grid re-load !
    // This isn't really bother us, since as soon as we have instanced BG-s, the whole map unloads as the BG gets ended
    if (!IsBattleGroundOrArena())
    {
        for (GridRefManager<NGridType>::iterator i = GridRefManager<NGridType>::begin(); i != GridRefManager<NGridType>::end(); )
        {
            NGridType *grid = i->getSource();
            GridInfo *info = i->getSource()->getGridInfoRef();
            ++i;                                                // The update might delete the map and we need the next map before the iterator gets invalid
            MANGOS_ASSERT(grid->GetGridState() >= 0 && grid->GetGridState() < MAX_GRID_STATE);
            UpdateGridState(*grid, *info, t_diff);
        }
    }

    ///- Process necessary scripts
    if (!m_scriptSchedule.empty())
        ScriptsProcess();

    if(i_data)
        i_data->Update(t_diff);

    m_weatherSystem->UpdateWeathers(t_diff);
}

void Map::Remove(Player* player, bool remove)
{
    if (!FindObject(player->GetObjectGuid()))
    {
        sLog.outError("Map::Remove: map %u instance %u %s not stored in Map object store, byt trying to be removed!", GetId(), GetInstanceId(), player->GetObjectGuid().GetString().c_str());
    }

    if (i_data)
        i_data->OnPlayerLeave(player);

    sLFGMgr.OnPlayerLeaveMap(player, this);

    RemoveFromActive(player);

    if (remove)
        player->CleanupsBeforeDelete();

    RemoveAttackersStorageFor(player->GetObjectGuid());

    player->RemoveFromWorld(remove);

    // this may be called during Map::Update
    player->GetMapRef().unlink();

    CellPair p = MaNGOS::ComputeCellPair(player->GetPositionX(), player->GetPositionY());
    if (p.x_coord >= TOTAL_NUMBER_OF_CELLS_PER_MAP || p.y_coord >= TOTAL_NUMBER_OF_CELLS_PER_MAP)
    {
        // invalid coordinates
        sLog.outError("Map::Remove player %s invalid cellPair x:%d, y:%d", player->GetObjectGuid().GetString().c_str(), p.x_coord,p.y_coord);
        remove ? DeleteFromWorld(player) :  (void)player->TeleportToHomebind();
        return;
    }

    Cell cell(p);
    NGridType* grid = getNGrid(cell.GridX(), cell.GridY());
    if (!grid)
    {
        // invalid coordinates
        sLog.outError("Map::Remove player %s i_grids was nullptr x:%d, y:%d",player->GetObjectGuid().GetString().c_str(),cell.data.Part.grid_x,cell.data.Part.grid_y);
        remove ? DeleteFromWorld(player) :  (void)player->TeleportToHomebind();
        return;
    }

    DEBUG_FILTER_LOG(LOG_FILTER_PLAYER_MOVES, "Map::Remove Remove player %s from grid [%u,%u]", player->GetName(), cell.GridX(), cell.GridY());

    RemoveFromGrid(player,grid,cell);
    SendRemoveActiveObjects(player);
    UpdateObjectVisibility(player,cell,p);

    player->ResetMap();

    // if remove - player be deleted (removed from storage already, in RemoveFromWorld()
    if (!remove)
        EraseObject(player->GetObjectGuid());
    else
        DeleteFromWorld(player);

    // if last player - set unload timer
    if (!IsContinent() && !IsBattleGroundOrArena())
    {
        if (!m_unloadTimer && !HavePlayers())
            m_unloadTimer = sWorld.getConfig(CONFIG_UINT32_INSTANCE_UNLOAD_DELAY);
    }
}

template<class T>
void
Map::Remove(T* obj, bool remove)
{
    CellPair p = MaNGOS::ComputeCellPair(obj->GetPositionX(), obj->GetPositionY());
    if(p.x_coord >= TOTAL_NUMBER_OF_CELLS_PER_MAP || p.y_coord >= TOTAL_NUMBER_OF_CELLS_PER_MAP )
    {
        sLog.outError("Map::Remove: Object (%s Type: %u) have invalid coordinates X:%f Y:%f grid cell [%u:%u]", obj->GetObjectGuid() ? obj->GetObjectGuid().GetString().c_str() : "<no GUID>", obj->GetTypeId(), obj->GetPositionX(), obj->GetPositionY(), p.x_coord, p.y_coord);
        return;
    }

    Cell cell(p);
    if( !loaded(GridPair(cell.data.Part.grid_x, cell.data.Part.grid_y)) )
        return;

    DEBUG_LOG("Remove object (GUID: %u TypeId:%u) from grid[%u,%u]", obj->GetGUIDLow(), obj->GetTypeId(), cell.data.Part.grid_x, cell.data.Part.grid_y);
    NGridType* grid = getNGrid(cell.GridX(), cell.GridY());
    MANGOS_ASSERT(grid != nullptr);

    RemoveFromActive(obj);

    if (remove)
        obj->CleanupsBeforeDelete();
    obj->RemoveFromWorld(remove);

    if (!obj->isActiveObject())
        UpdateObjectVisibility(obj,cell,p);                     // i think will be better to call this function while object still in grid, this changes nothing but logically is better(as for me)

    RemoveFromGrid(obj,grid,cell);

    if (obj->GetTypeId() == TYPEID_UNIT)
        RemoveAttackersStorageFor(obj->GetObjectGuid());

    if (obj->isActiveObject())
        SendRemoveNotifyToStoredClients(obj, bool(obj->GetTypeId() == TYPEID_UNIT));

    if (remove)
    {
        // if option set then object already saved at this moment
        if(!sWorld.getConfig(CONFIG_BOOL_SAVE_RESPAWN_TIME_IMMEDIATELY))
            obj->SaveRespawnTime();

        obj->ResetMap();
        // Note: In case resurrectable corpse and pet its removed from global lists in own destructor
        delete obj;
    }
    else
    {
        EraseObject(obj->GetObjectGuid());
    }
}

template<class T>
void Map::Relocation(T* obj, Position const& /*pos*/)
{
    sLog.outError("Map::Relocation unhandled relocation call (object %s)!", obj ? obj->GetObjectGuid().GetString().c_str() : "<none>");
    MANGOS_ASSERT(false);
};

template<>
void Map::Relocation(Player* player, Position const& pos)
{
    MANGOS_ASSERT(player);

    CellPair old_val = MaNGOS::ComputeCellPair(player->GetPositionX(), player->GetPositionY());
    CellPair new_val = MaNGOS::ComputeCellPair(pos.x, pos.y);

    Cell old_cell(old_val);
    Cell new_cell(new_val);
    bool same_cell = (new_cell == old_cell);

    player->Relocate(pos);

    if( old_cell.DiffGrid(new_cell) || old_cell.DiffCell(new_cell) )
    {
        DEBUG_FILTER_LOG(LOG_FILTER_PLAYER_MOVES, "Player %s relocation grid[%u,%u]cell[%u,%u]->grid[%u,%u]cell[%u,%u]", player->GetName(), old_cell.GridX(), old_cell.GridY(), old_cell.CellX(), old_cell.CellY(), new_cell.GridX(), new_cell.GridY(), new_cell.CellX(), new_cell.CellY());

        NGridType* oldGrid = getNGrid(old_cell.GridX(), old_cell.GridY());
        RemoveFromGrid(player, oldGrid,old_cell);
        if( !old_cell.DiffGrid(new_cell) )
            AddToGrid(player, oldGrid,new_cell);
        else
            EnsureGridLoadedAtEnter(new_cell, player);

        NGridType* newGrid = getNGrid(new_cell.GridX(), new_cell.GridY());
        player->GetViewPoint().Event_GridChanged(&(*newGrid)(new_cell.CellX(),new_cell.CellY()));
    }

    player->OnRelocated();

    if (!same_cell)
        ActivateGrid(getNGrid(new_cell.GridX(), new_cell.GridY()));
};

template<>
void Map::Relocation(Creature* creature, Position const& pos)
{
    MANGOS_ASSERT(CheckGridIntegrity(creature,false));

//    Cell old_cell = creature->GetCurrentCell();
    Cell new_cell(MaNGOS::ComputeCellPair(pos.x, pos.y));

    // do move or do move to respawn or remove creature if previous all fail
    if (CreatureCellRelocation(creature,new_cell))
    {
        // update pos
        creature->Relocate(pos);
        creature->OnRelocated();
    }
    // if creature can't be move in new cell/grid (not loaded) move it to repawn cell/grid
    // creature coordinates will be updated and notifiers send
    else if (!CreatureRespawnRelocation(creature))
    {
        // ... or unload (if respawn grid also not loaded)
        DEBUG_FILTER_LOG(LOG_FILTER_CREATURE_MOVES, "Creature (GUID: %u Entry: %u ) can't be move to unloaded respawn grid.",creature->GetGUIDLow(),creature->GetEntry());
    }

    MANGOS_ASSERT(CheckGridIntegrity(creature,true));
};

template<>
void Map::Relocation(GameObject* go, Position const& pos)
{
    MANGOS_ASSERT(go);

    CellPair old_val = MaNGOS::ComputeCellPair(go->GetPositionX(), go->GetPositionY());
    CellPair new_val = MaNGOS::ComputeCellPair(pos.x, pos.y);

    Cell old_cell(old_val);
    Cell new_cell(new_val);

    go->Relocate(pos);

    if (old_cell != new_cell)
    {
        // Need check for spawn some GO in not loaded grids
        EnsureGridLoadedAtEnter(old_cell);

        NGridType* oldGrid = getNGrid(old_cell.GridX(), old_cell.GridY());

        RemoveFromGrid(go, oldGrid,old_cell);
        EnsureGridLoadedAtEnter(new_cell);
        NGridType* newGrid = getNGrid(new_cell.GridX(), new_cell.GridY());
        ActivateGrid(newGrid);

        AddToGrid(go, newGrid, new_cell);

        go->GetViewPoint().Event_GridChanged(&(*newGrid)(new_cell.CellX(),new_cell.CellY()));
        DEBUG_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES, "GO %s moved in grid[%u,%u] from cell[%u,%u] to cell[%u,%u].", go->GetObjectGuid().GetString().c_str(), old_cell.GridX(), old_cell.GridY(), old_cell.CellX(), old_cell.CellY(), new_cell.CellX(), new_cell.CellY());
    }

    go->UpdateObjectVisibility();
    // go->OnRelocated();
};

void Map::CreatureRelocation(Creature* object, float x, float y, float z, float orientation)
{
    Relocation(object, Position(x, y, z, orientation, object->GetPhaseMask()));
};

bool Map::CreatureCellRelocation(Creature* c, Cell new_cell)
{
    Cell const& old_cell = c->GetCurrentCell();
    if (old_cell.DiffGrid(new_cell))
    {
        if (!c->isActiveObject() && !loaded(new_cell.gridPair()))
        {
            DEBUG_FILTER_LOG(LOG_FILTER_CREATURE_MOVES, "Creature (GUID: %u Entry: %u) attempt move from grid[%u,%u]cell[%u,%u] to unloaded grid[%u,%u]cell[%u,%u].", c->GetGUIDLow(), c->GetEntry(), old_cell.GridX(), old_cell.GridY(), old_cell.CellX(), old_cell.CellY(), new_cell.GridX(), new_cell.GridY(), new_cell.CellX(), new_cell.CellY());
            return false;
        }
        EnsureGridLoadedAtEnter(new_cell);
    }

    if (old_cell != new_cell)
    {
        DEBUG_FILTER_LOG(LOG_FILTER_CREATURE_MOVES, "Creature (GUID: %u Entry: %u) moved in grid[%u,%u] from cell[%u,%u] to cell[%u,%u].", c->GetGUIDLow(), c->GetEntry(), old_cell.GridX(), old_cell.GridY(), old_cell.CellX(), old_cell.CellY(), new_cell.CellX(), new_cell.CellY());
        NGridType* oldGrid = getNGrid(old_cell.GridX(), old_cell.GridY());
        NGridType* newGrid = getNGrid(new_cell.GridX(), new_cell.GridY());
        RemoveFromGrid(c, oldGrid, old_cell);
        if (c->isActiveObject())
            ActivateGrid(newGrid);
        AddToGrid(c, newGrid, new_cell);
        c->GetViewPoint().Event_GridChanged(&(*newGrid)(new_cell.CellX(),new_cell.CellY()));
    }
    return true;
}

bool Map::CreatureRespawnRelocation(Creature *c)
{
    WorldLocation loc = c->GetRespawnCoord();

    CellPair resp_val = MaNGOS::ComputeCellPair(loc.x, loc.y);
    Cell resp_cell(resp_val);

    c->CombatStop();
    c->GetMotionMaster()->Clear();

    DEBUG_FILTER_LOG(LOG_FILTER_CREATURE_MOVES, "Creature (GUID: %u Entry: %u) will moved from grid[%u,%u]cell[%u,%u] to respawn grid[%u,%u]cell[%u,%u].", c->GetGUIDLow(), c->GetEntry(), c->GetCurrentCell().GridX(), c->GetCurrentCell().GridY(), c->GetCurrentCell().CellX(), c->GetCurrentCell().CellY(), resp_cell.GridX(), resp_cell.GridY(), resp_cell.CellX(), resp_cell.CellY());

    // teleport it to respawn point (like normal respawn if player see)
    if(CreatureCellRelocation(c,resp_cell))
    {
        c->Relocate(loc);
        c->GetMotionMaster()->Initialize();                 // prevent possible problems with default move generators
        c->OnRelocated();
        return true;
    }
    else
        return false;
}

bool Map::UnloadGrid(NGridType& grid, bool pForce)
{
    if (!pForce && ActiveObjectsNearGrid(grid.getX(), grid.getY()))
        return false;

    SetGridObjectDataLoaded(false, grid);
    ObjectGridUnloader unloader(grid);

    // Finish remove and delete all creatures with delayed remove before moving to respawn grids
    // Must know real mob position before move
    RemoveAllObjectsInRemoveList();

    // move creatures to respawn grids if this is diff.grid or to remove list
    unloader.MoveToRespawnN();

    // Finish remove and delete all creatures with delayed remove before unload
    RemoveAllObjectsInRemoveList();

    unloader.UnloadN();
    setNGrid(nullptr, grid.getX(), grid.getY());

    DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "Map::UnloadGrid unloading grid[%u,%u]"
                     " for map %u finished", grid.getX(), grid.getY(), GetId() );

    int gx = (MAX_NUMBER_OF_GRIDS - 1) - grid.getX();
    int gy = (MAX_NUMBER_OF_GRIDS - 1) - grid.getY();

    // unload GridMap - it is reference-countable so will be deleted safely when lockCount < 1
    // also simply set Map's pointer to corresponding GridMap object to nullptr
    if (m_bLoadedGrids[gx][gy])
    {
        m_bLoadedGrids[gx][gy] = false;
        m_TerrainData->Unload(gx, gy);
    }

    return true;
}

void Map::UnloadAll(bool pForce)
{
    while (!IsLoadingObjectsQueueEmpty())
    {
        if (LoadingObjectQueueMember* member = GetNextLoadingObject())
            delete member;
    }

    for (GridRefManager<NGridType>::iterator i = GridRefManager<NGridType>::begin(); i != GridRefManager<NGridType>::end(); )
    {
        NGridType& grid(*i->getSource());
        ++i;
        UnloadGrid(grid, pForce);       // deletes the grid and removes it from the GridRefManager
    }

    //if (GetInstanceId() && !sMapMgr.IsTransportMap(GetId()))
    //    sObjectMgr.UnLoadTransports(this);

    DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "Map::UnloadAll unloading all objects from map %u instance %u complete", GetId(), GetInstanceId());
}

void Map::AddLoadingObject(LoadingObjectQueueMember* obj)
{
    i_loadingObjectQueue.push(obj);
}

LoadingObjectQueueMember* Map::GetNextLoadingObject()
{
    LoadingObjectQueueMember* loadingObject = nullptr;
    if (!IsLoadingObjectsQueueEmpty())
    {
        loadingObject = i_loadingObjectQueue.top();
        i_loadingObjectQueue.pop();
    }
    return loadingObject;
}

MapDifficultyEntry const* Map::GetMapDifficulty() const
{
    return GetMapDifficultyData(GetId(), GetDifficulty());
}

uint32 Map::GetMaxPlayers() const
{
    MapDifficultyEntry const* mapDiff = GetMapDifficulty();
    if (mapDiff && mapDiff->maxPlayers)
        return mapDiff->maxPlayers;

    return i_mapEntry->maxPlayers;
}

uint32 Map::GetMaxResetDelay() const
{
    return DungeonResetScheduler::GetMaxResetTimeFor(GetMapDifficulty());
}

bool Map::CheckGridIntegrity(Creature* c, bool moved) const
{
    Cell const& cur_cell = c->GetCurrentCell();

    CellPair xy_val = MaNGOS::ComputeCellPair(c->GetPositionX(), c->GetPositionY());
    Cell xy_cell(xy_val);
    if(xy_cell != cur_cell)
    {
        sLog.outError("Creature (GUIDLow: %u) X: %f Y: %f (%s) in grid[%u,%u]cell[%u,%u] instead grid[%u,%u]cell[%u,%u]",
            c->GetGUIDLow(),
            c->GetPositionX(),c->GetPositionY(),(moved ? "final" : "original"),
            cur_cell.GridX(), cur_cell.GridY(), cur_cell.CellX(), cur_cell.CellY(),
            xy_cell.GridX(),  xy_cell.GridY(),  xy_cell.CellX(),  xy_cell.CellY());
        return true;                                        // not crash at error, just output error in debug mode
    }

    return true;
}

const char* Map::GetMapName() const
{
    return i_mapEntry ? i_mapEntry->name[sWorld.GetDefaultDbcLocale()] : "UNNAMEDMAP\x0";
}

void UpdateObjectVisibilityWithHelper::operator() (WorldObject* object) const
{
    if (!object)
        return;

    m_obj->GetMap()->UpdateObjectVisibility(object, m_cell, m_cellpair);
}

void Map::UpdateObjectVisibility(WorldObject* obj, Cell cell, CellPair cellpair)
{
    cell.SetNoCreate();
    MaNGOS::VisibleChangesNotifier notifier(*obj);
    TypeContainerVisitor<MaNGOS::VisibleChangesNotifier, WorldTypeMapContainer > player_notifier(notifier);
    cell.Visit(cellpair, player_notifier, *this, *obj, GetVisibilityDistance(obj));
    if (obj->IsTransport() && obj->GetTransportBase() && obj->GetTransportBase()->HasPassengers())
        obj->GetTransportBase()->CallForAllPassengers(UpdateObjectVisibilityWithHelper(obj, cell, cellpair));
}

void Map::SendInitSelf(Player* player )
{
    DETAIL_LOG("Creating player data for himself %u", player->GetGUIDLow());

    UpdateData data;
    // build data for self presence in world at own client (one time for map)
    player->BuildCreateUpdateBlockForPlayer(&data, player);

    // build other passengers at transport also (they always visible and marked as visible and will not send at visibility update at add to map
    if (Transport* transport = player->GetTransport())
    {
        // attach to player data current transport data
        transport->BuildCreateUpdateBlockForPlayer(&data, player);
        transport->GetTransportKit()->CallForAllPassengers(SendCurrentTransportDataWithHelper(&data, player));
    }

    WorldPacket packet;
    data.BuildPacket(&packet);
    player->GetSession()->SendPacket(&packet);
}

void Map::SendInitActiveObjects(Player* player)
{
    if (!player)
        return;

    GuidQueue updateQueue = GetActiveObjects();
    if (updateQueue.empty())
        return;

    UpdateData initData = UpdateData();
    bool hasAny = false;

    while(!updateQueue.empty())
    {
        ObjectGuid guid = updateQueue.front();
        updateQueue.pop();

        if (guid.IsPlayer())
            continue;

        WorldObject* object = GetWorldObject(guid);

        if (!object || !object->IsInWorld() || !object->isVisibleForInState(player,player,false) || !object->isActiveObject())
            continue;

        object->BuildCreateUpdateBlockForPlayer(&initData, player);
        object->AddNotifiedClient(player->GetObjectGuid());
        hasAny = true;
        DEBUG_FILTER_LOG(LOG_FILTER_VISIBILITY_CHANGES, "Map::SendInitActiveObjects %s visibility initialized for %s",
                      guid.GetString().c_str(), player->GetGuidStr().c_str());
    }
    if (!hasAny)
        return;

    WorldPacket packet;
    initData.BuildPacket(&packet);
    player->GetSession()->SendPacket(&packet);
}

void Map::SendRemoveActiveObjects(Player* player)
{
    if (!player)
        return;

    GuidQueue updateQueue = GetActiveObjects();
    if (updateQueue.empty())
        return;

    UpdateData initData = UpdateData();
    bool hasAny = false;

    while(!updateQueue.empty())
    {
        ObjectGuid guid = updateQueue.front();
        updateQueue.pop();

        if (guid.IsPlayer())
            continue;

        WorldObject* object = GetWorldObject(guid);

        if (!object || !object->IsInWorld() || !object->isActiveObject())
            continue;

        object->BuildOutOfRangeUpdateBlock(&initData);
        hasAny = true;
        object->RemoveNotifiedClient(player->GetObjectGuid());
        DEBUG_FILTER_LOG(LOG_FILTER_VISIBILITY_CHANGES, "Map::SendRemoveActiveObjects %s visibility removed for %s",
                    guid.GetString().c_str(), player->GetGuidStr().c_str());
    }
    if (!hasAny)
        return;

    WorldPacket packet;
    initData.BuildPacket(&packet);
    player->GetSession()->SendPacket(&packet);
}

inline void Map::setNGrid(NGridType* grid, uint32 x, uint32 y)
{
    if(x >= MAX_NUMBER_OF_GRIDS || y >= MAX_NUMBER_OF_GRIDS)
    {
        sLog.outError("map::setNGrid() Invalid grid coordinates found: %d, %d!",x,y);
        MANGOS_ASSERT(false);
    }
    i_grids[x][y] = grid;
}

void Map::AddObjectToRemoveList(WorldObject* obj, bool /*immediateCleanup*/)
{
    MANGOS_ASSERT(obj && obj->GetMap() == this);

    obj->CleanupsBeforeDelete();                                // remove or simplify at least cross referenced links

    i_objectsToRemove.insert(obj);
    //DEBUG_LOG("Object (GUID: %u TypeId: %u ) added to removing list.",obj->GetGUIDLow(),obj->GetTypeId());
}

void Map::RemoveObjectFromRemoveList(WorldObject* obj)
{
    if (!i_objectsToRemove.empty())
        i_objectsToRemove.erase(obj);
}

void Map::RemoveAllObjectsInRemoveList()
{
    if(i_objectsToRemove.empty())
        return;

    //DEBUG_LOG("Object remover 1 check.");
    while(!i_objectsToRemove.empty())
    {
        WorldObject* obj = *i_objectsToRemove.begin();
        i_objectsToRemove.erase(i_objectsToRemove.begin());

        if (!obj)
            continue;

        switch(obj->GetTypeId())
        {
            case TYPEID_CORPSE:
                Remove((Corpse*)obj,true);
                break;
            case TYPEID_DYNAMICOBJECT:
                Remove((DynamicObject*)obj,true);
                break;
            case TYPEID_GAMEOBJECT:
                Remove((GameObject*)obj,true);
                break;
            case TYPEID_UNIT:
                Remove((Creature*)obj,true);
                break;
            default:
                sLog.outError("Non-grid object (TypeId: %u) in grid object removing list, ignored.",obj->GetTypeId());
                break;
        }
    }
    //DEBUG_LOG("Object remover 2 check.");
}

uint32 Map::GetPlayersCountExceptGMs() const
{
    uint32 count = 0;
    for(MapRefManager::const_iterator itr = m_mapRefManager.begin(); itr != m_mapRefManager.end(); ++itr)
        if(!itr->getSource()->isGameMaster())
            ++count;
    return count;
}

void Map::SendToPlayers(WorldPacket const* data) const
{
    for(MapRefManager::const_iterator itr = m_mapRefManager.begin(); itr != m_mapRefManager.end(); ++itr)
        itr->getSource()->GetSession()->SendPacket(data);
}

bool Map::ActiveObjectsNearGrid(uint32 x, uint32 y) const
{
    MANGOS_ASSERT(x < MAX_NUMBER_OF_GRIDS);
    MANGOS_ASSERT(y < MAX_NUMBER_OF_GRIDS);

    CellPair cell_min(x*MAX_NUMBER_OF_CELLS, y*MAX_NUMBER_OF_CELLS);
    CellPair cell_max(cell_min.x_coord + MAX_NUMBER_OF_CELLS, cell_min.y_coord+MAX_NUMBER_OF_CELLS);

    //we must find visible range in cells so we unload only non-visible cells...
    float viewDist = GetVisibilityDistance();
    int cell_range = (int)ceil(viewDist / SIZE_OF_GRID_CELL) + 1;

    cell_min << cell_range;
    cell_min -= cell_range;
    cell_max >> cell_range;
    cell_max += cell_range;

    GuidQueue updateQueue = const_cast<Map*>(this)->GetActiveObjects();
    if (updateQueue.empty())
        return false;

    while(!updateQueue.empty())
    {
        ObjectGuid guid = updateQueue.front();
        updateQueue.pop();

        WorldObject const* obj = const_cast<Map*>(this)->GetWorldObject(guid);
        if (!obj || !obj->isActiveObject())
            continue;

        CellPair p = MaNGOS::ComputeCellPair(obj->GetPositionX(), obj->GetPositionY());
        if( (cell_min.x_coord <= p.x_coord && p.x_coord <= cell_max.x_coord) &&
            (cell_min.y_coord <= p.y_coord && p.y_coord <= cell_max.y_coord) )
            return true;
    }

    return false;
}

void Map::AddToActive(WorldObject* obj)
{
    if (!obj)
        return;

    m_activeObjects.insert(obj->GetObjectGuid());

    Cell cell = Cell(MaNGOS::ComputeCellPair(obj->GetPositionX(), obj->GetPositionY()));
    EnsureGridLoadedAtEnter(cell, obj->GetTypeId() == TYPEID_PLAYER ? (Player*)obj : nullptr);

    // also not allow unloading spawn grid to prevent creating creature clone at load
    if (obj->GetTypeId() == TYPEID_UNIT)
    {
        Creature* c= (Creature*)obj;

        if (!c->IsPet() && c->HasStaticDBSpawnData())
        {
            float x,y,z;
            c->GetRespawnCoord(x,y,z);
            GridPair p = MaNGOS::ComputeGridPair(x, y);
            if(getNGrid(p.x_coord, p.y_coord))
                getNGrid(p.x_coord, p.y_coord)->incUnloadActiveLock();
            else
            {
                GridPair p2 = MaNGOS::ComputeGridPair(c->GetPositionX(), c->GetPositionY());
                sLog.outError("Active creature (GUID: %u Entry: %u) added to grid[%u,%u] but spawn grid[%u,%u] not loaded.",
                    c->GetGUIDLow(), c->GetEntry(), p.x_coord, p.y_coord, p2.x_coord, p2.y_coord);
            }
        }
    }

    if (obj->GetTypeId() != TYPEID_PLAYER && IsVisibleGlobally(obj->GetObjectGuid()))
    {
        PlayerList const& plist = GetPlayers();
        if (!plist.isEmpty())
        {
            WorldPacket packet;
            for (PlayerList::const_iterator itr = plist.begin(); itr != plist.end(); ++itr)
            {
                Player* player = itr->getSource();
                if (player && player->IsInWorld())
                {
                    UpdateData data;
                    obj->BuildCreateUpdateBlockForPlayer(&data, player);
                    data.BuildPacket(&packet);
                    player->SendDirectMessage(&packet);
                    obj->AddNotifiedClient(player->GetObjectGuid());
                    DEBUG_FILTER_LOG(LOG_FILTER_VISIBILITY_CHANGES, "Map::AddToActive %s send visibility notify to %s",
                        obj->GetObjectGuid().GetString().c_str(), player->GetObjectGuid().GetString().c_str());
                }
            }
        }
    }
}

void Map::RemoveFromActive(WorldObject* obj)
{
    if (!obj)
        return;

    // Map::Update for active object in proccess
    m_activeObjects.erase(obj->GetObjectGuid());

    // also allow unloading spawn grid
    if (obj->GetTypeId() == TYPEID_UNIT)
    {
        Creature* c = (Creature*)obj;

        if(!c->IsPet() && c->HasStaticDBSpawnData())
        {
            float x,y,z;
            c->GetRespawnCoord(x,y,z);
            GridPair p = MaNGOS::ComputeGridPair(x, y);
            if(getNGrid(p.x_coord, p.y_coord))
                getNGrid(p.x_coord, p.y_coord)->decUnloadActiveLock();
            else
            {
                GridPair p2 = MaNGOS::ComputeGridPair(c->GetPositionX(), c->GetPositionY());
                sLog.outError("Active creature (GUID: %u Entry: %u) removed from grid[%u,%u] but spawn grid[%u,%u] not loaded.",
                    c->GetGUIDLow(), c->GetEntry(), p.x_coord, p.y_coord, p2.x_coord, p2.y_coord);
            }
        }
    }
}

void Map::CreateInstanceData(bool load)
{
    if(i_data != nullptr)
        return;

    if (Instanceable())
    {
        if (InstanceTemplate const* mInstance = ObjectMgr::GetInstanceTemplate(GetId()))
            i_script_id = mInstance->script_id;
    }
    else
    {
        if (WorldTemplate const* mInstance = ObjectMgr::GetWorldTemplate(GetId()))
            i_script_id = mInstance->script_id;
    }

    if (!i_script_id)
        return;

    i_data = sScriptMgr.CreateInstanceData(this);
    if(!i_data)
        return;

    if (load)
    {
        // TODO: make a global storage for this
        QueryResult* result;

        if (Instanceable())
            result = CharacterDatabase.PQuery("SELECT data FROM instance WHERE id = '%u'", i_InstanceId);
        else
            result = CharacterDatabase.PQuery("SELECT data FROM world WHERE map = '%u'", GetId());

        if (result)
        {
            Field* fields = result->Fetch();
            const char* data = fields[0].GetString();
            if (data)
            {
                DEBUG_LOG("Loading instance data for `%s` (Map: %u Instance: %u)", sScriptMgr.GetScriptName(i_script_id), GetId(), i_InstanceId);
                i_data->Load(data);
            }
            delete result;
        }
        else
        {
            // for non-instanceable map always add data to table if not found, later code expected that for map in `word` exist always after load
            if (!Instanceable())
                CharacterDatabase.PExecute("INSERT INTO world VALUES ('%u', '')", GetId());
        }
    }
    else
    {
        DEBUG_LOG("New instance data, \"%s\" ,initialized!", sScriptMgr.GetScriptName(i_script_id));
        i_data->Initialize();
    }
}

template void Map::Add(Corpse *);
template void Map::Add(Creature *);
template void Map::Add(GameObject *);
template void Map::Add(DynamicObject *);

template void Map::Remove(Corpse *,bool);
template void Map::Remove(Creature *,bool);
template void Map::Remove(GameObject *, bool);
template void Map::Remove(DynamicObject *, bool);

/* ******* World Maps ******* */

WorldPersistentState* WorldMap::GetPersistanceState() const
{
    return (WorldPersistentState*)Map::GetPersistentState();
}

/* ******* Dungeon Instance Maps ******* */

DungeonMap::DungeonMap(uint32 id, time_t expiry, uint32 InstanceId, uint8 SpawnMode)
  : Map(id, expiry, InstanceId, SpawnMode),
    m_resetAfterUnload(false), m_unloadWhenEmpty(false)
{
    MANGOS_ASSERT(i_mapEntry->IsDungeon());

    //lets initialize visibility distance for dungeons
    DungeonMap::InitVisibilityDistance();
}

DungeonMap::~DungeonMap()
{
}

void DungeonMap::InitVisibilityDistance()
{
    //init visibility distance for instances
    m_VisibleDistance = World::GetMaxVisibleDistanceInInstances();
}

/*
    Do map specific checks to see if the player can enter
*/
bool DungeonMap::CanEnter(Player *player)
{
    if(player->GetMapRef().getTarget() == this)
    {
        sLog.outError("DungeonMap::CanEnter - player %s(%u) already in map %d,%d,%d!", player->GetName(), player->GetGUIDLow(), GetId(), GetInstanceId(), GetSpawnMode());
        MANGOS_ASSERT(false);
        return false;
    }

    return Map::CanEnter(player);
}

/*
    Do map specific checks and add the player to the map if successful.
*/
bool DungeonMap::Add(Player *player)
{
    // TODO: Not sure about checking player level: already done in HandleAreaTriggerOpcode
    // GMs still can teleport player in instance.
    // Is it needed?

    if (!CanEnter(player))
        return false;

    // check for existing instance binds
    InstancePlayerBind *playerBind = player->GetBoundInstance(GetId(), GetDifficulty());
    if (playerBind && playerBind->perm)
    {
        // cannot enter other instances if bound permanently
        if (playerBind->state != GetPersistanceState())
        {
            sLog.outError("DungeonMap::Add: player %s(%d) is permanently bound to instance %d,%d,%d,%d,%d,%d but he is being put in instance %d,%d,%d,%d,%d,%d",
                player->GetName(), player->GetGUIDLow(), playerBind->state->GetMapId(),
                playerBind->state->GetInstanceId(), playerBind->state->GetDifficulty(),
                playerBind->state->GetPlayerCount(), playerBind->state->GetGroupCount(),
                playerBind->state->CanReset(),
                GetPersistanceState()->GetMapId(), GetPersistanceState()->GetInstanceId(),
                GetPersistanceState()->GetDifficulty(), GetPersistanceState()->GetPlayerCount(),
                GetPersistanceState()->GetGroupCount(), GetPersistanceState()->CanReset());
            //MANGOS_ASSERT(false);
            player->RemoveFromGroup();
            player->RepopAtGraveyard();
            player->GetSession()->KickPlayer();
            return false;
        }
    }
    else
    {
        Group *pGroup = player->GetGroup();
        if (pGroup)
        {
            // solo saves should be reset when entering a group
            InstanceGroupBind* groupBind = pGroup->GetBoundInstance(this,GetDifficulty());
            if (playerBind)
            {
                DEBUG_LOG("DungeonMap::Add: %s enter to instance %d,%d,%d,%d,%d,%d but he is in group (Id: %d) and has non-permanent bind to instance %d,%d,%d,%d,%d,%d!",
                    player->GetGuidStr().c_str(), GetPersistentState()->GetMapId(), GetPersistentState()->GetInstanceId(),
                    GetPersistanceState()->GetDifficulty(), GetPersistanceState()->GetPlayerCount(), GetPersistanceState()->GetGroupCount(),
                    GetPersistanceState()->CanReset(), pGroup->GetId(),
                    playerBind->state->GetMapId(), playerBind->state->GetInstanceId(), playerBind->state->GetDifficulty(),
                    playerBind->state->GetPlayerCount(), playerBind->state->GetGroupCount(), playerBind->state->CanReset());

                if (groupBind)
                    DEBUG_LOG("DungeonMap::Add: the group (Id: %d) is bound to instance %d,%d,%d,%d,%d,%d.",
                        pGroup->GetId(),
                        groupBind->state->GetMapId(), groupBind->state->GetInstanceId(), groupBind->state->GetDifficulty(),
                        groupBind->state->GetPlayerCount(), groupBind->state->GetGroupCount(), groupBind->state->CanReset());

                // no reason crash if we can fix state
                player->UnbindInstance(GetId(), GetDifficulty());
            }

            // bind to the group or keep using the group save
            if (!groupBind)
                pGroup->BindToInstance(GetPersistanceState(), false);
            else
            {
                // cannot jump to a different instance without resetting it
                if (groupBind->state != GetPersistentState())
                {
                    DEBUG_LOG("DungeonMap::Add: %s is being put in instance %d,%d,%d but he is in group (Id: %d) which has non-permanent bind to instance %d,%d,%d!",
                        player->GetGuidStr().c_str(), GetPersistentState()->GetMapId(),
                        GetPersistentState()->GetInstanceId(), GetPersistentState()->GetDifficulty(),
                        pGroup->GetId(), groupBind->state->GetMapId(),
                        groupBind->state->GetInstanceId(), groupBind->state->GetDifficulty());

                    sLog.outError("MapSave players: %d, group count: %d",
                        GetPersistanceState()->GetPlayerCount(), GetPersistanceState()->GetGroupCount());

                    if (groupBind->state)
                        sLog.outError("GroupBind save players: %d, group count: %d", groupBind->state->GetPlayerCount(), groupBind->state->GetGroupCount());
                    else
                        sLog.outError("GroupBind save nullptr");
                    //MANGOS_ASSERT(false);
                    player->RemoveFromGroup();
                    player->RepopAtGraveyard();
                    player->GetSession()->KickPlayer();
                    return false;
                }
                // if the group/leader is permanently bound to the instance
                // players also become permanently bound when they enter
                if (groupBind->perm && IsDungeon())
                {
                    WorldPacket data(SMSG_PENDING_RAID_LOCK, 9);
                    data << uint32(60000);
                    data << groupBind->state->GetCompletedEncountersMask();
                    data << uint8(0);
                    player->GetSession()->SendPacket(&data);
                    player->SetPendingBind(GetPersistanceState(), 60000);
                }
            }
        }
        else
        {
            // set up a solo bind or continue using it
            if(!playerBind)
                player->BindToInstance(GetPersistanceState(), false);
            else
            {
                // cannot jump to a different instance without resetting it
                MANGOS_ASSERT(playerBind->state == GetPersistentState());
            }
        }
    }

    // for normal instances cancel the reset schedule when the
    // first player enters (no players yet)
    SetResetSchedule(false);

    DETAIL_LOG("MAP: Player '%s' is entering instance '%u' of map '%s'", player->GetName(), GetInstanceId(), GetMapName());
    // initialize unload state
    m_resetAfterUnload = false;
    m_unloadWhenEmpty = false;

    // this will acquire the same mutex so it cannot be in the previous block
    Map::Add(player);

    return true;
}

void DungeonMap::Update(const uint32& t_diff)
{
    Map::Update(t_diff);
}

void DungeonMap::Remove(Player *player, bool remove)
{
    DETAIL_LOG("MAP: Removing player '%s' from instance '%u' of map '%s' before relocating to other map", player->GetName(), GetInstanceId(), GetMapName());

    Map::Remove(player, remove);

    // if last player and unload when empty - unload immediately
    if (m_unloadWhenEmpty && !HavePlayers())
        m_unloadTimer = MIN_UNLOAD_DELAY;

    // for normal instances schedule the reset after all players have left
    SetResetSchedule(true);
}

/*
    Returns true if there are no players in the instance
*/
bool DungeonMap::Reset(InstanceResetMethod method)
{
    // note: since the map may not be loaded when the instance needs to be reset
    // the instance must be deleted from the DB by InstanceSaveManager

    if(HavePlayers())
    {
        if (method == INSTANCE_RESET_ALL)
        {
            // notify the players to leave the instance so it can be reset
            for(MapRefManager::iterator itr = m_mapRefManager.begin(); itr != m_mapRefManager.end(); ++itr)
                itr->getSource()->SendResetFailedNotify(GetId());
        }
        else if (method == INSTANCE_RESET_CHANGE_DIFFICULTY)
        {
            // notify the players to leave the instance so it can be reset
            for(MapRefManager::iterator itr = m_mapRefManager.begin(); itr != m_mapRefManager.end(); ++itr)
                itr->getSource()->SendResetFailedNotify(GetId());

            m_unloadWhenEmpty = true;
            m_resetAfterUnload = true;
        }
        else
        {
            if(method == INSTANCE_RESET_GLOBAL)
            {
                // set the homebind timer for players inside (1 minute)
                for(MapRefManager::iterator itr = m_mapRefManager.begin(); itr != m_mapRefManager.end(); ++itr)
                    itr->getSource()->m_InstanceValid = false;
            }

            // the unload timer is not started
            // instead the map will unload immediately after the players have left
            m_unloadWhenEmpty = true;
            m_resetAfterUnload = true;
        }
    }
    else
    {
        // unloaded at next update
        m_unloadTimer = MIN_UNLOAD_DELAY;
        m_resetAfterUnload = true;
    }

    return m_mapRefManager.isEmpty();
}

void DungeonMap::PermBindAllPlayers(Player *player, bool permanent)
{
    Group *group = player->GetGroup();
    // group members outside the instance group don't get bound
    for(MapRefManager::iterator itr = m_mapRefManager.begin(); itr != m_mapRefManager.end(); ++itr)
    {
        Player* plr = itr->getSource();
        // players inside an instance cannot be bound to other instances
        // some players may already be permanently bound, in this case nothing happens
        InstancePlayerBind *bind = plr->GetBoundInstance(GetId(), GetDifficulty());
        if (!bind || !bind->perm)
        {
            plr->BindToInstance(GetPersistanceState(), permanent);
            WorldPacket data(SMSG_INSTANCE_SAVE_CREATED, 4);
            data << uint32(0);
            plr->GetSession()->SendPacket(&data);
            sCalendarMgr.SendCalendarRaidLockoutAdd(plr->GetObjectGuid(), GetPersistanceState());
        }

        // if the leader is not in the instance the group will not get a perm bind
        if (group && group->GetLeaderGuid() == plr->GetObjectGuid())
            group->BindToInstance(GetPersistanceState(), permanent);
    }
}

void DungeonMap::UnloadAll(bool pForce)
{
    if(HavePlayers())
    {
        sLog.outError("DungeonMap::UnloadAll: there are still players in the instance at unload, should not happen!");
        for(MapRefManager::iterator itr = m_mapRefManager.begin(); itr != m_mapRefManager.end(); ++itr)
        {
            Player* plr = itr->getSource();
            plr->TeleportToHomebind();
        }
    }

    if(m_resetAfterUnload == true)
    {
        if (DungeonPersistentState* state = GetPersistanceState())
            state->DeleteRespawnTimes();
    }

    Map::UnloadAll(pForce);
}

void DungeonMap::SendResetWarnings(uint32 timeLeft) const
{
    for(MapRefManager::const_iterator itr = m_mapRefManager.begin(); itr != m_mapRefManager.end(); ++itr)
        itr->getSource()->SendInstanceResetWarning(GetId(), itr->getSource()->GetDifficulty(IsRaid()), timeLeft);
}

void DungeonMap::SetResetSchedule(bool on)
{
    // only for normal instances
    // the reset time is only scheduled when there are no payers inside
    // it is assumed that the reset time will rarely (if ever) change while the reset is scheduled
    if(!HavePlayers() && !IsRaidOrHeroicDungeon())
        sMapPersistentStateMgr.GetScheduler().ScheduleReset(on, GetPersistanceState()->GetResetTime(), DungeonResetEvent(RESET_EVENT_NORMAL_DUNGEON, GetId(), Difficulty(GetSpawnMode()), GetInstanceId()));
}

DungeonPersistentState* DungeonMap::GetPersistanceState() const
{
    return (DungeonPersistentState*)Map::GetPersistentState();
}


/* ******* Battleground Instance Maps ******* */

BattleGroundMap::BattleGroundMap(uint32 id, time_t expiry, uint32 InstanceId, uint8 spawnMode)
  : Map(id, expiry, InstanceId, spawnMode)
{
    //lets initialize visibility distance for BG/Arenas
    BattleGroundMap::InitVisibilityDistance();
}

BattleGroundMap::~BattleGroundMap()
{
}

void BattleGroundMap::Update(const uint32& diff)
{
    Map::Update(diff);

    GetBG()->Update(diff);
}

BattleGroundPersistentState* BattleGroundMap::GetPersistanceState() const
{
    return (BattleGroundPersistentState*)Map::GetPersistentState();
}


void BattleGroundMap::InitVisibilityDistance()
{
    //init visibility distance for BG/Arenas
    m_VisibleDistance = World::GetMaxVisibleDistanceInBGArenas();
}

bool BattleGroundMap::CanEnter(Player * player)
{
    if(player->GetMapRef().getTarget() == this)
    {
        sLog.outError("BGMap::CanEnter - player %u already in map!", player->GetGUIDLow());
        MANGOS_ASSERT(false);
        return false;
    }

    if(player->GetBattleGroundId() != GetInstanceId())
        return false;

    // player number limit is checked in bgmgr, no need to do it here

    return Map::CanEnter(player);
}

bool BattleGroundMap::Add(Player * player)
{
    if(!CanEnter(player))
        return false;

    // reset instance validity, battleground maps do not homebind
    player->m_InstanceValid = true;

    return Map::Add(player);
}

void BattleGroundMap::Remove(Player *player, bool remove)
{
    DETAIL_LOG("MAP: Removing player '%s' from bg '%u' of map '%s' before relocating to other map", player->GetName(), GetInstanceId(), GetMapName());
    Map::Remove(player, remove);
}

void BattleGroundMap::SetUnload()
{
    m_unloadTimer = MIN_UNLOAD_DELAY;
}

void BattleGroundMap::UnloadAll(bool pForce)
{
    while(HavePlayers())
    {
        if(Player * plr = m_mapRefManager.getFirst()->getSource())
        {
            plr->TeleportTo(plr->GetBattleGroundEntryPoint(), TELE_TO_NODELAY);
            // TeleportTo removes the player from this map (if the map exists) -> calls BattleGroundMap::Remove -> invalidates the iterator.
            // just in case, remove the player from the list explicitly here as well to prevent a possible infinite loop
            // note that this remove is not needed if the code works well in other places
            plr->GetMapRef().unlink();
        }
    }

    Map::UnloadAll(pForce);
}

/// Put scripts in the execution queue
bool Map::ScriptsStart(ScriptMapMapName const& scripts, uint32 id, Object* source, Object* target, ScriptExecutionParam execParams /*=SCRIPT_EXEC_PARAM_UNIQUE_BY_SOURCE_TARGET*/)
{
    MANGOS_ASSERT(source);

    ///- Find the script map
    ScriptMapMap::const_iterator s = scripts.second.find(id);
    if (s == scripts.second.end())
        return false;

    // prepare static data
    ObjectGuid sourceGuid = source->GetObjectGuid();
    ObjectGuid targetGuid = target ? target->GetObjectGuid() : ObjectGuid();
    ObjectGuid ownerGuid  = source->isType(TYPEMASK_ITEM) ? ((Item*)source)->GetOwnerGuid() : ObjectGuid();

    if (execParams)                                         // Check if the execution should be uniquely
    {
        for (ScriptScheduleMap::const_iterator searchItr = m_scriptSchedule.begin(); searchItr != m_scriptSchedule.end(); ++searchItr)
        {
            if (searchItr->second.IsSameScript(scripts.first, id,
                    execParams & SCRIPT_EXEC_PARAM_UNIQUE_BY_SOURCE ? sourceGuid : ObjectGuid(),
                    execParams & SCRIPT_EXEC_PARAM_UNIQUE_BY_TARGET ? targetGuid : ObjectGuid(), ownerGuid))
            {
                DEBUG_LOG("DB-SCRIPTS: Process table `%s` id %u. Skip script as script already started for source %s, target %s - ScriptsStartParams %u", scripts.first, id, sourceGuid.GetString().c_str(), targetGuid.GetString().c_str(), execParams);
                return true;
            }
        }
    }

    ///- Schedule script execution for all scripts in the script map
    ScriptMap const *s2 = &(s->second);
    for (ScriptMap::const_iterator iter = s2->begin(); iter != s2->end(); ++iter)
    {
        ScriptAction sa(scripts.first, this, sourceGuid, targetGuid, ownerGuid, &iter->second);
        sScriptMgr.IncreaseScheduledScriptsCount();
        m_scriptSchedule.insert(ScriptScheduleMap::value_type(time_t(sWorld.GetGameTime() + iter->first), sa));
    }

    return true;
}

void Map::ScriptCommandStart(ScriptInfo const& script, uint32 delay, Object* source, Object* target)
{
    // NOTE: script record _must_ exist until command executed

    // prepare static data
    ObjectGuid sourceGuid = source->GetObjectGuid();
    ObjectGuid targetGuid = target ? target->GetObjectGuid() : ObjectGuid();
    ObjectGuid ownerGuid  = source->isType(TYPEMASK_ITEM) ? ((Item*)source)->GetOwnerGuid() : ObjectGuid();

    ScriptAction sa("Internal Activate Command used for spell", this, sourceGuid, targetGuid, ownerGuid, &script);
    sScriptMgr.IncreaseScheduledScriptsCount();

    m_scriptSchedule.insert(ScriptScheduleMap::value_type(time_t(sWorld.GetGameTime() + delay), sa));
}

/// Process queued scripts
void Map::ScriptsProcess()
{
    // ok as multimap is a *sorted* associative container
    while (ScriptAction* action = GetNextSheduledScript())
    {
        if (action->HandleScriptStep())
        {
            // Terminate following script steps of this script
            const char* tableName = action->GetTableName();
            uint32 id = action->GetId();
            ObjectGuid sourceGuid = action->GetSourceGuid();
            ObjectGuid targetGuid = action->GetTargetGuid();
            ObjectGuid ownerGuid = action->GetOwnerGuid();

            for (ScriptScheduleMap::iterator rmItr = m_scriptSchedule.begin(); rmItr != m_scriptSchedule.end(); ++rmItr)
            {
                if (rmItr->second.IsSameScript(tableName, id, sourceGuid, targetGuid, ownerGuid))
                {
                    if (EraseScriptAction(&rmItr->second))
                        rmItr = m_scriptSchedule.begin();
                }
            }
        }
        else
            EraseScriptAction(action);
    }
}

ScriptAction* Map::GetNextSheduledScript()
{
    if (m_scriptSchedule.empty())
        return nullptr;
    ScriptScheduleMap::iterator iter = m_scriptSchedule.begin();
    return (iter->first <= sWorld.GetGameTime()) ? &iter->second : nullptr;
}

bool Map::EraseScriptAction(ScriptAction* action)
{
    if (m_scriptSchedule.empty())
        return false;
    for (ScriptScheduleMap::iterator itr = m_scriptSchedule.begin(); itr != m_scriptSchedule.end(); ++itr)
    {
        if (&itr->second == action)
        {
            m_scriptSchedule.erase(itr);
            sScriptMgr.DecreaseScheduledScriptCount();
            return true;
        }
    }
    return false;
}

/**
 * Function inserts any object in MapObjectStore
 *
 * @param guid must be WorldObject*
 */
void Map::InsertObject(WorldObject* object)
{
    if (!object)
        return;

    m_objectsStore.insert(MapStoredObjectTypesContainer::value_type(object->GetObjectGuid(), object));
}

void Map::EraseObject(WorldObject* object)
{
    if (!object)
        return;

    EraseObject(object->GetObjectGuid());
}

void Map::EraseObject(ObjectGuid guid)
{
    if (guid.IsEmpty())
        return;

    m_objectsStore.erase(guid);
}

WorldObject* Map::FindObject(ObjectGuid guid)
{
    if (guid.IsEmpty())
        return nullptr;

    MapStoredObjectTypesContainer::iterator itr = m_objectsStore.find(guid);
    return (itr == m_objectsStore.end()) ? nullptr : itr->second;
}

/**
 * Function return player that in world at CURRENT map
 *
 * Note: This is function preferred if you sure that need player only placed at specific map
 *       This is not true for some spell cast targeting and most packet handlers
 *
 * @param guid must be player guid (HIGHGUID_PLAYER)
 */
Player* Map::GetPlayer(ObjectGuid guid, bool globalSearch)
{
    if (globalSearch)                                           // possible obsolete
    {
        Player* plr = ObjectAccessor::FindPlayer(guid);         // return only in world players
        return plr && plr->GetMap() == this ? plr : nullptr;
    }
    else
        return (Player*)FindObject(guid);
}

/**
 * Function return creature (non-pet and then most summoned by spell creatures) that in world at CURRENT map
 *
 * @param guid must be creature or vehicle guid (HIGHGUID_UNIT HIGHGUID_VEHICLE)
 */
Creature* Map::GetCreature(ObjectGuid guid)
{
    return (Creature*)FindObject(guid);
}

/**
 * Function return pet that in world at CURRENT map
 *
 * @param guid must be pet guid (HIGHGUID_PET)
 */
Pet* Map::GetPet(ObjectGuid guid)
{
    return (Pet*)FindObject(guid);
}

/**
 * Function return corpse that at CURRENT map
 *
 * Note: corpse can be NOT IN WORLD, so can't be used corpse->GetMap() without pre-check corpse->isInWorld()
 *
 * @param guid must be corpse guid (HIGHGUID_CORPSE)
 */
Corpse* Map::GetCorpse(ObjectGuid guid)
{
    Corpse * ret = ObjectAccessor::GetCorpseInMap(guid, GetId());
    return ret && ret->GetInstanceId() == GetInstanceId() ? ret : nullptr;
}

/**
 * Function return non-player unit object that in world at CURRENT map, so creature, or pet, or vehicle
 *
 * @param guid must be non-player unit guid (HIGHGUID_PET HIGHGUID_UNIT HIGHGUID_VEHICLE)
 */
Creature* Map::GetAnyTypeCreature(ObjectGuid guid)
{
    switch (guid.GetHigh())
    {
        case HIGHGUID_UNIT:
        case HIGHGUID_VEHICLE:
        case HIGHGUID_PET:
            return (Creature*)FindObject(guid);
        default:
            break;
    }
    return nullptr;
}

/**
 * Function return gameobject that in world at CURRENT map
 *
 * @param guid must be gameobject guid (HIGHGUID_GAMEOBJECT)
 */
GameObject* Map::GetGameObject(ObjectGuid guid)
{
    return (GameObject*)FindObject(guid);
}

/**
 * Function return dynamic object that in world at CURRENT map
 *
 * @param guid must be dynamic object guid (HIGHGUID_DYNAMICOBJECT)
 */
DynamicObject* Map::GetDynamicObject(ObjectGuid guid)
{
    return (DynamicObject*)FindObject(guid);
}

/**
 * Function return transport that in world at CURRENT map
 *
 * @param guid must be dynamic object guid (HIGHGUID_MO_TRANSPORT)
 */
Transport* Map::GetTransport(ObjectGuid guid)
{
    return (Transport*)FindObject(guid);
}

/**
 * Function return unit in world at CURRENT map
 *
 * Note: in case player guid not always expected need player at current map only.
 *       For example in spell casting can be expected any in world player targeting in some cases
 *
 * @param guid must be unit guid (HIGHGUID_PLAYER HIGHGUID_PET HIGHGUID_UNIT HIGHGUID_VEHICLE)
 */
Unit* Map::GetUnit(ObjectGuid guid)
{
    if (guid.IsPlayer())
        return GetPlayer(guid);

    return GetAnyTypeCreature(guid);
}

/**
 * Function return world object in world at CURRENT map, so any except transports
 */
WorldObject* Map::GetWorldObject(ObjectGuid guid)
{
    switch(guid.GetHigh())
    {
        case HIGHGUID_PLAYER:
        case HIGHGUID_GAMEOBJECT:
        case HIGHGUID_UNIT:
        case HIGHGUID_VEHICLE:
        case HIGHGUID_PET:
        case HIGHGUID_DYNAMICOBJECT:
        case HIGHGUID_MO_TRANSPORT:
        case HIGHGUID_TRANSPORT:
            return FindObject(guid);
        case HIGHGUID_CORPSE:
        {
            // corpse special case, it can be not in world
            Corpse* corpse = GetCorpse(guid);
            return corpse && corpse->IsInWorld() ? corpse : nullptr;
        }
        default:
            break;
    }

    return nullptr;
}

void Map::AddUpdateObject(ObjectGuid guid)
{
    i_objectsToClientUpdate.insert(guid);
}

void Map::RemoveUpdateObject(ObjectGuid guid)
{
    i_objectsToClientUpdate.erase(guid);
}

ObjectGuid Map::GetNextObjectFromUpdateQueue()
{
    ObjectGuid guid = i_objectsToClientUpdate.empty() ? ObjectGuid::Null : *i_objectsToClientUpdate.begin();
    i_objectsToClientUpdate.erase(guid);
    return guid;
}

void Map::SendObjectUpdates()
{
    UpdateDataMapType update_players = UpdateDataMapType();

    while (ObjectGuid guid = GetNextObjectFromUpdateQueue())
    {
        WorldObject* obj = GetWorldObject(guid);
        if (obj && obj->IsInWorld())
        {
            if (obj->IsMarkedForClientUpdate())
                obj->BuildUpdateData(update_players);
            if (obj->GetObjectsUpdateQueue() && !obj->GetObjectsUpdateQueue()->empty())
            {
                while (!obj->GetObjectsUpdateQueue()->empty())
                {
                    ObjectGuid dependentGuid = *obj->GetObjectsUpdateQueue()->begin();
                    obj->RemoveUpdateObject(dependentGuid);
                    Object* dependentObj = obj->GetDependentObject(dependentGuid);
                    if (dependentObj && dependentObj->IsMarkedForClientUpdate())
                        dependentObj->BuildUpdateData(update_players);
                }
            }
        }
    }

    if (!update_players.empty())
    {
        WorldPacket packet;
        for (UpdateDataMapType::iterator iter = update_players.begin(); iter != update_players.end(); ++iter)
        {
            if (!iter->first || !iter->first.IsPlayer())
                continue;

            Player* pPlayer = GetPlayer(iter->first);
            if (!pPlayer || !pPlayer->GetSession())
                continue;

            iter->second.BuildPacket(&packet);
            pPlayer->GetSession()->SendPacket(&packet);
        }
    }
}

uint32 Map::GenerateLocalLowGuid(HighGuid guidhigh)
{
    // TODO: for map local guid counters possible force reload map instead shutdown server at guid counter overflow
    switch(guidhigh)
    {
        case HIGHGUID_UNIT:
        case HIGHGUID_VEHICLE:
            return m_CreatureGuids.Generate();
        case HIGHGUID_GAMEOBJECT:
        case HIGHGUID_MO_TRANSPORT:
        case HIGHGUID_TRANSPORT:
            return m_GameObjectGuids.Generate();
        case HIGHGUID_DYNAMICOBJECT:
            return m_DynObjectGuids.Generate();
        case HIGHGUID_PET:
            return m_PetGuids.Generate();
        default:
            MANGOS_ASSERT(false);
            return 0;
    }
}

/**
 * Helper structure for building static chat information
 *
 */
class StaticMonsterChatBuilder
{
    public:
        StaticMonsterChatBuilder(CreatureInfo const* cInfo, ChatMsg msgtype, int32 textId, Language language, Unit const* target, uint32 senderLowGuid = 0)
            : i_cInfo(cInfo), i_msgtype(msgtype), i_textId(textId), i_language(language), i_target(target)
        {
            // 0 lowguid not used in core, but accepted fine in this case by client
            i_senderGuid = i_cInfo->GetObjectGuid(senderLowGuid);
        }
        void operator()(WorldPacket& data, int32 loc_idx)
        {
            char const* text = sObjectMgr.GetMangosString(i_textId, loc_idx);

            char const* nameForLocale = i_cInfo->Name;
            sObjectMgr.GetCreatureLocaleStrings(i_cInfo->Entry, loc_idx, &nameForLocale);

            ChatHandler::BuildChatPacket(data, i_msgtype, text, i_language, CHAT_TAG_NONE, i_senderGuid, nameForLocale, i_target ? i_target->GetObjectGuid() : ObjectGuid(),
                i_target ? i_target->GetNameForLocaleIdx(loc_idx) : "");
        }

    private:
        ObjectGuid i_senderGuid;
        CreatureInfo const* i_cInfo;
        ChatMsg i_msgtype;
        int32 i_textId;
        Language i_language;
        Unit const* i_target;
};


/**
 * Function simulates yell of creature
 *
 * @param guid must be creature guid of whom to Simulate the yell, non-creature guids not supported at this moment
 * @param textId Id of the simulated text
 * @param language language of the text
 * @param target, can be nullptr
 */
void Map::MonsterYellToMap(ObjectGuid guid, int32 textId, Language language, Unit const* target) const
{
    if (guid.IsAnyTypeCreature())
    {
        CreatureInfo const* cInfo = ObjectMgr::GetCreatureTemplate(guid.GetEntry());
        if (!cInfo)
        {
            sLog.outError("Map::MonsterYellToMap: Called for nonexistent creature entry in guid: %s", guid.GetString().c_str());
            return;
        }

        MonsterYellToMap(cInfo, textId, language, target, guid.GetCounter());
    }
    else
    {
        sLog.outError("Map::MonsterYellToMap: Called for non creature guid: %s", guid.GetString().c_str());
        return;
    }
}


/**
 * Function simulates yell of creature
 *
 * @param cinfo must be entry of Creature of whom to Simulate the yell
 * @param textId Id of the simulated text
 * @param language language of the text
 * @param target, can be nullptr
 * @param senderLowGuid provide way proper show yell for near spawned creature with known lowguid,
 *        0 accepted by client else if this not important
 */
void Map::MonsterYellToMap(CreatureInfo const* cinfo, int32 textId, Language language, Unit const* target, uint32 senderLowGuid /*= 0*/) const
{
    StaticMonsterChatBuilder say_build(cinfo, CHAT_MSG_MONSTER_YELL, textId, language, target, senderLowGuid);
    MaNGOS::LocalizedPacketDo<StaticMonsterChatBuilder> say_do(say_build);

    Map::PlayerList const& pList = GetPlayers();
    for (PlayerList::const_iterator itr = pList.begin(); itr != pList.end(); ++itr)
        say_do(itr->getSource());
}

/**
 * Function to play sound to all players in map
 *
 * @param soundId Played Sound
 * @param zoneId Id of the Zone to which the sound should be restricted
 */
void Map::PlayDirectSoundToMap(uint32 soundId, uint32 zoneId /*=0*/) const
{
    WorldPacket data(SMSG_PLAY_SOUND, 4);
    data << uint32(soundId);

    Map::PlayerList const& pList = GetPlayers();
    for (PlayerList::const_iterator itr = pList.begin(); itr != pList.end(); ++itr)
        if (!zoneId || itr->getSource()->GetZoneId() == zoneId)
            itr->getSource()->SendDirectMessage(&data);
}

/**
* Function to change weather in zone and change weather on map from script.
*
* need base support (With Values 0)
*/

void Map::SetWeather(uint32 zoneId, WeatherType type, float grade, bool permanently)
{
    Weather* wth = m_weatherSystem->FindOrCreateWeather(zoneId);
    wth->SetWeather(WeatherType(type), grade, this, permanently);
}

void Map::UpdateWorldState(uint32 state, uint32 value)
{
    sWorldStateMgr.SetWorldStateValueFor(this, state, value);
}

/**
 * Function to operations with attackers per-map storage
 *
 * @param targetGuid (attackerGuid)
 */

void Map::AddAttackerFor(ObjectGuid const& targetGuid, ObjectGuid const& attackerGuid)
{
    if (targetGuid.IsEmpty() || attackerGuid.IsEmpty())
        return;

    AttackersMap::iterator itr = m_attackersMap.find(targetGuid);
    if (itr != m_attackersMap.end())
    {
        itr->second.insert(attackerGuid);
    }
    else
    {
        CreateAttackersStorageFor(targetGuid);
        m_attackersMap[targetGuid].insert(attackerGuid);
    }
}

void Map::RemoveAttackerFor(ObjectGuid const& targetGuid, ObjectGuid const& attackerGuid)
{
    if (targetGuid.IsEmpty() || attackerGuid.IsEmpty())
        return;

    AttackersMap::iterator itr = m_attackersMap.find(targetGuid);
    if (itr != m_attackersMap.end())
    {
        itr->second.erase(attackerGuid);
    }
}

void Map::RemoveAllAttackersFor(ObjectGuid const& targetGuid)
{
    if (targetGuid.IsEmpty())
        return;

    AttackersMap::iterator itr = m_attackersMap.find(targetGuid);
    if (itr != m_attackersMap.end())
    {
        itr->second.clear();
    }
}

GuidSet& Map::GetAttackersFor(ObjectGuid const& targetGuid)
{
    return m_attackersMap[targetGuid];
}

bool Map::IsInCombat(ObjectGuid const& targetGuid) const
{
    AttackersMap::const_iterator itr = m_attackersMap.find(targetGuid);
    if (itr == m_attackersMap.end())
        return false;

    return !itr->second.empty();
}

void Map::CreateAttackersStorageFor(ObjectGuid const& targetGuid)
{
    if (targetGuid.IsEmpty())
        return;

    AttackersMap::iterator itr = m_attackersMap.find(targetGuid);
    if (itr == m_attackersMap.end())
    {
        m_attackersMap.insert(std::make_pair(targetGuid,GuidSet()));
    }

}

void Map::RemoveAttackersStorageFor(ObjectGuid const& targetGuid)
{
    if (targetGuid.IsEmpty())
        return;

    AttackersMap::iterator itr = m_attackersMap.find(targetGuid);
    if (itr != m_attackersMap.end())
    {
        m_attackersMap.erase(itr);
    }
}

void Map::ForcedUnload()
{
    sLog.outError("Map::ForcedUnload called for map %u instance %u. Map crushed. Cleaning up...", GetId(), GetInstanceId());

    // Immediately cleanup update queue
    i_objectsToClientUpdate.clear();

    Map::PlayerList const pList = GetPlayers();
    for (PlayerList::const_iterator itr = pList.begin(); itr != pList.end(); ++itr)
    {
        Player* player = itr->getSource();
        if (!player || !player->GetSession())
            continue;

        if (player->IsBeingTeleportedFar())
        {
            WorldLocation old_loc = player->GetPosition();
            if (!player->TeleportTo(old_loc, TELE_TO_NODELAY))
            {
                DETAIL_LOG("Map::ForcedUnload: %s is in teleport state, cannot be ported to his previous place, teleporting him to his homebind place...",
                    player->GetGuidStr().c_str());
                player->TeleportToHomebind();
            }
            player->SetSemaphoreTeleportFar(false);
        }

        switch (sWorld.getConfig(CONFIG_UINT32_VMSS_MAPFREEMETHOD))
        {
            case 0:
            {
                player->RemoveAllAurasOnDeath();
                if (Pet* pet = player->GetPet())
                    pet->RemoveAllAurasOnDeath();
                player->GetSession()->LogoutPlayer(true);
                break;
            }
            case 1:
            {
                player->GetSession()->KickPlayer();
                Remove(player, true);
                break;
            }
            case 2:
            {
                player->GetSession()->LogoutPlayer(false);
                Remove(player, true);
                break;
            }
            default:
                break;
        }
    }

    switch (sWorld.getConfig(CONFIG_UINT32_VMSS_MAPFREEMETHOD))
    {
        case 0:
            if (InstanceData* iData = GetInstanceData())
                iData->Save();
            break;
        case 1:
        case 2:
        default:
            break;
    }

    UnloadAll(true);

    RemoveAllObjectsInRemoveList();

    SetBroken(false);
}

float Map::GetVisibilityDistance(WorldObject const* obj) const
{
    if (obj && obj->GetTypeId() == TYPEID_GAMEOBJECT)
        return (m_VisibleDistance + ((GameObject const*)obj)->GetDeterminativeSize());
    else
        return m_VisibleDistance;
}

bool Map::IsInLineOfSight(float srcX, float srcY, float srcZ, float destX, float destY, float destZ, uint32 phasemask) const
{
    return VMAP::VMapFactory::createOrGetVMapManager()->isInLineOfSight(GetId(), srcX, srcY, srcZ, destX, destY, destZ)
        && IsInLineOfSightByDynamicMapTree(srcX, srcY, srcZ, destX, destY, destZ, phasemask);
}

/**
test if we hit an object. return true if we hit one. the dest position will hold the orginal dest position or the possible hit position
return true if we hit something
*/
// return HitPosition in dest point
bool Map::GetHitPosition(float srcX, float srcY, float srcZ, float& destX, float& destY, float& destZ, uint32 phasemask, float modifyDist) const
{
    // at first check all static objects
    float tempX, tempY, tempZ = 0.0f;
    bool result0 = VMAP::VMapFactory::createOrGetVMapManager()->getObjectHitPos(GetId(), srcX, srcY, srcZ, destX, destY, destZ, tempX, tempY, tempZ, modifyDist);
    if (result0)
    {
        DEBUG_LOG("Map::GetHitPosition vmaps corrects gained with static objects! new dest coords are X:%f Y:%f Z:%f",destX, destY, destZ);
        destX = tempX;
        destY = tempY;
        destZ = tempZ;
    }
    // at second all dynamic objects, if static check has an hit, then we can calculate only to this point and NOT to end, because we need closely hit point
    bool result1 = GetDynamicMapTree().getObjectHitPos(phasemask, srcX, srcY, srcZ, destX, destY, destZ, tempX, tempY, tempZ, modifyDist);
    if (result1)
    {
        DEBUG_LOG("Map::GetHitPosition vmaps corrects gained with dynamic objects! new dest coords are X:%f Y:%f Z:%f",destX, destY, destZ);
        destX = tempX;
        destY = tempY;
        destZ = tempZ;
    }
    return result0 || result1;
}

float Map::GetHeight(uint32 phasemask, float x, float y, float z) const
{
    float staticHeight = m_TerrainData->GetHeightStatic(x, y, z);

    // Get Dynamic Height around static Height (if valid)
    float dynSearchHeight = 2.0f + (z < staticHeight ? staticHeight : z);
    return std::max<float>(staticHeight, GetDynamicMapTree().getHeight(x, y, dynSearchHeight, dynSearchHeight - staticHeight, phasemask));
}

// Find an height within a reasonable range of provided Z. This method may fail so we have to handle that case.
bool Map::GetHeightInRange(uint32 phasemask, float x, float y, float &z, float maxSearchDist /*= 4.0f*/) const
{
    float height, vmapHeight, mapHeight = 0.0f;
    vmapHeight = VMAP_INVALID_HEIGHT_VALUE;

    VMAP::IVMapManager* vmgr = VMAP::VMapFactory::createOrGetVMapManager();
    if (!vmgr->isLineOfSightCalcEnabled())
        vmgr = nullptr;

    if (vmgr)
    {
        // pure vmap search
        vmapHeight = vmgr->getHeight(i_id, x, y, z + 2.0f, maxSearchDist + 2.0f);
    }

    // find raw height from .map file on X,Y coordinates
    GridMap* gmap = const_cast<TerrainInfo*>(m_TerrainData)->GetGrid(x, y);
    if ( gmap != nullptr ) // TODO:: find a way to remove that const_cast
        mapHeight = gmap->getHeight(x, y);

    float diffMaps = fabs(fabs(z) - fabs(mapHeight));
    float diffVmaps = fabs(fabs(z) - fabs(vmapHeight));
    if (diffVmaps < maxSearchDist)
    {
        if (diffMaps < maxSearchDist)
        {
            // well we simply have to take the highest as normally there we cannot be on top of cavern is maxSearchDist is not too big
            if (vmapHeight > mapHeight)
                height = vmapHeight;
            else
                height = mapHeight;

            //sLog.outString("vmap %5.4f, map %5.4f, height %5.4f", vmapHeight, mapHeight, height);
        }
        else
        {
            //sLog.outString("vmap %5.4f", vmapHeight);
            height = vmapHeight;
        }
    }
    else
    {
        if (diffMaps < maxSearchDist)
        {
            //sLog.outString("map %5.4f", mapHeight);
            height = mapHeight;
        }
        else
            return false;
    }

    z = std::max<float>(height, m_dyn_tree.getHeight(x, y, height + 1.0f, maxSearchDist, phasemask));
    return true;
}

void Map::InsertGameObjectModel(GameObjectModel const& mdl)
{
    m_dyn_tree.insert(mdl);
}

void Map::RemoveGameObjectModel(GameObjectModel const& mdl)
{
    m_dyn_tree.remove(mdl);
}

bool Map::ContainsGameObjectModel(GameObjectModel const& mdl) const
{
    return GetDynamicMapTree().contains(mdl);
}

void Map::DynamicMapTreeBalance()
{
    m_dyn_tree.balance();
}

void Map::DynamicMapTreeUpdate(uint32 const& t_diff)
{
    m_dyn_tree.update(t_diff);
}

bool Map::IsInLineOfSightByDynamicMapTree(float srcX, float srcY, float srcZ, float destX, float destY, float destZ, uint32 phasemask) const
{
    return GetDynamicMapTree().isInLineOfSight(srcX, srcY, srcZ, destX, destY, destZ, phasemask);
}

DynamicMapTree const& Map::GetDynamicMapTree() const
{
   // Don't need lock here! use locks around this usage.
   return m_dyn_tree;
}

template<class T> void Map::LoadObjectToGrid(uint32& guid, GridType& grid, BattleGround* bg)
{
    T* obj = new T;
    if(!obj->LoadFromDB(guid, this))
    {
        delete obj;
        return;
    }

    grid.AddGridObject(obj);

    setUnitCell(obj);

    obj->SetMap(this);
    obj->AddToWorld();
    if (obj->isActiveObject())
        AddToActive(obj);

    obj->GetViewPoint().Event_AddedToWorld(&grid);

    if (bg)
        bg->OnObjectDBLoad(obj);

    obj->UpdateObjectVisibility();
}

WorldObjectEventProcessor* Map::GetEvents()
{
    return &m_Events;
}

void Map::KillAllEvents(bool force)
{
    GetEvents()->KillAllEvents(force);
}

void Map::AddEvent(BasicEvent* Event, uint64 e_time, bool set_addtime)
{
    if (set_addtime)
        GetEvents()->AddEvent(Event, GetEvents()->CalculateTime(e_time), set_addtime);
    else
        GetEvents()->AddEvent(Event, e_time, set_addtime);
}

void Map::UpdateEvents(uint32 update_diff)
{
    GetEvents()->RenewEvents();
    GetEvents()->Update(update_diff);
}

bool Map::IsVisibleGlobally(ObjectGuid const& guid)
{
    if (guid.IsMOTransport())
    {
        return bool(FindObject(guid));
    }
    return false;
}

void Map::SendRemoveNotifyToStoredClients(WorldObject* object, bool destroy)
{
    if (!object || !object->HasNotifiedClients())
        return;

    WorldPacket out_packet(SMSG_DESTROY_OBJECT, 9);
    if (destroy)
    {
        out_packet << object->GetObjectGuid();
        out_packet << uint8(0);
    }
    else
    // Packet type rewrited to SMSG_UPDATE_OBJECT here
    {
        UpdateData data;
        object->BuildOutOfRangeUpdateBlock(&data);
        data.BuildPacket(&out_packet);
    }

    uint32 count = 0;

    for (GuidSet::const_iterator itr = object->GetNotifiedClients().begin(); itr != object->GetNotifiedClients().end(); ++itr)
    {
        if (Player* player = GetPlayer(*itr))
        {
            if (player->IsInWorld() && !player->HaveAtClient(object->GetObjectGuid()))
            {
                player->SendDirectMessage(&out_packet);
                ++count;
            }
        }
    }

    DEBUG_FILTER_LOG(LOG_FILTER_VISIBILITY_CHANGES, "Map::SendRemoveNotifyToStoredClients "
                     "%s send visibility notify to " SIZEFMTD " clients (%u really)",
                     object->GetObjectGuid().GetString().c_str(),
                     object->GetNotifiedClients().size(), count);

    object->GetNotifiedClients().clear();
}

bool Map::UpdateGridState(NGridType& grid, GridInfo& info, uint32 const& t_diff)
{
    switch (grid.GetGridState())
    {
        case GRID_STATE_ACTIVE:
        {
            // Only check grid activity every (grid_expiry/10) ms, because it's really useless to do it every cycle
            info.UpdateTimeTracker(t_diff);
            if (info.getTimeTracker().Passed())
            {
                if (grid.ActiveObjectsInGrid() == 0 && !ActiveObjectsNearGrid(grid.getX(), grid.getY()))
                {
                    ObjectGridStoper stoper(grid);
                    stoper.StopN();
                    grid.SetGridState(GRID_STATE_IDLE);
                    DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING,"Map::UpdateGridState grid[%u,%u] on map %u instance %u moved to IDLE state", grid.getX(), grid.getY(), GetId(), GetInstanceId());
                    ResetGridExpiry(grid, 0.1f);
                }
                else
                {
                    ResetGridExpiry(grid, 0.2f);
                }
            }
            break;
        }
        case GRID_STATE_IDLE:
        {
            // Only check grid activity very (grid_expiry/10) ms, because it's really useless to do it every cycle
            info.UpdateTimeTracker(t_diff);
            if (info.getTimeTracker().Passed())
            {
                ResetGridExpiry(grid, 0.8f);
                grid.SetGridState(GRID_STATE_REMOVAL);
                DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING,"Map::UpdateGridState grid[%u,%u] on map %u instance %u moved to REMOVAL state", grid.getX(), grid.getY(), GetId(), GetInstanceId());
            }
            break;
        }
        case GRID_STATE_REMOVAL:
        {
            if (!info.getUnloadLock())
            {
                info.UpdateTimeTracker(t_diff);
                if (info.getTimeTracker().Passed())
                {
                    if (!UnloadGrid(grid, false))
                    {
                        DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING,"Map::UpdateGridState grid[%u,%u] for map %u instance %u differed unloading due to players or active objects nearby", grid.getX(), grid.getY(), GetId(), GetInstanceId());
                        ResetGridExpiry(grid);
                    }
                }
            }
            break;
        }
        case GRID_STATE_INVALID:
        case MAX_GRID_STATE:
        default:
        {
            sLog.outError("Map::UpdateGridState trying set invalid state (%u) to grid[%u,%u] on map %u instance %u", grid.GetGridState(), grid.getX(), grid.getY(), GetId(), GetInstanceId());
            return false;
        }
    }
    return true;
}

GuidQueue Map::GetActiveObjects()
{
    GuidQueue result;
    for (GuidSet::const_iterator itr = m_activeObjects.begin(); itr != m_activeObjects.end(); ++itr)
        result.push(*itr);
    return result;
}

time_t Map::GetGridExpiry() const
{
    return sWorld.getConfig(CONFIG_UINT32_INTERVAL_GRIDCLEAN);
}

bool Map::GetRandomPointUnderWater(uint32 phaseMask, float& x, float& y, float& z, float radius, float water_z)
{
    const float angle = rand_norm_f() * (M_PI_F * 2.0f);
    const float range = rand_norm_f() * radius;

    float i_x = x + range * cos(angle);
    float i_y = y + range * sin(angle);

    // get real ground of new point
    // the code considere cylindre instead of sphere for possible z
    float ground = GetHeight(phaseMask, i_x, i_y, z) + 0.5f;     // always a little bit above the ground
    if (ground > INVALID_HEIGHT)    // GetHeight can fail
    {
        // compute up/down max/min offset
        float z1 = radius + z;
        if (z1 > water_z)
            z1 = water_z;
        z1 = fabs(z1);

        float min_z = z - radius;
        if (min_z < ground)
            min_z = ground;
        float z2 = fabs(min_z);

        x = i_x;
        y = i_y;

        uint32 usable_range = (z1 > z2) ? z1 - z2 : z2 - z1;    // rounded to uint32

        float z_offset = rand_norm_f() * usable_range;
        z = min_z + z_offset;

        return true;
    }
    return false;
}

bool Map::GetRandomPointInTheAir(uint32 phaseMask, float& x, float& y, float& z, float radius)
{
    const float angle = rand_norm_f() * (M_PI_F * 2.0f);
    const float range = rand_norm_f() * radius;

    float i_x = x + range * cos(angle);
    float i_y = y + range * sin(angle);

    // get real ground of new point
    // the code considere cylindre instead of sphere for possible z
    float ground = GetHeight(phaseMask, i_x, i_y, z);
    if (ground > INVALID_HEIGHT)    // GetHeight can fail
    {
        float min_z = z - radius;
        if (min_z < ground)
            min_z = ground + 0.5f;
        float z1 = fabs(min_z);
        float z2 = fabs(z + radius);

        uint32 usable_range = (z1 > z2) ? z1 - z2 : z2 - z1;    // rounded to unit32

        x = i_x;
        y = i_y;
        z = min_z + rand_norm_f() * float(usable_range);
        return true;
    }
    return false;
}

// supposed to be used for not big radius, usually less than 20.0f
bool Map::GetReachableRandomPointOnGround(uint32 phaseMask, float& x, float& y, float& z, float radius)
{
    // Generate a random range and direction for the new point
    const float angle = rand_norm_f() * (M_PI_F * 2.0f);
    const float range = rand_norm_f() * radius;

    float i_x = x + range * cos(angle);
    float i_y = y + range * sin(angle);
    float i_z = z + 1.0f;

    GetHitPosition(x, y, z + 1.0f, i_x, i_y, i_z, phaseMask, -0.5f);
    i_z = z; // reset i_z to z value to avoid too much difference from original point before GetHeightInRange
    if (!GetHeightInRange(phaseMask, i_x, i_y, i_z)) // GetHeight can fail
        return false;

    // here we have a valid position but the point can have a big Z in some case
    // next code will check angle from 2 points
    //        c
    //       /|
    //      / |
    //    b/__|a

    // project vector to get only positive value
    float ab = fabs(x - i_x);
    float ac = fabs(z - i_z);

    // slope represented by c angle (in radian)
    float slope = 0;
    const float MAX_SLOPE_IN_RADIAN = 50.0f / 180.0f * M_PI_F;  // 50(degree) max seem best value for walkable slope

    // check ab vector to avoid divide by 0
    if (ab > 0.0f)
    {
        // compute c angle and convert it from radian to degree
        slope = atan(ac / ab);
        if (slope < MAX_SLOPE_IN_RADIAN)
        {
            x = i_x;
            y = i_y;
            z = i_z;
            return true;
        }
    }

    return false;
}

// Get random point by handling different situation depending of if the unit is flying/swimming/walking
bool Map::GetReachableRandomPosition(Unit* unit, float& x, float& y, float& z, float radius)
{

    float i_x = x;
    float i_y = y;
    float i_z = z;

    bool newDestAssigned = false;   // used to check if new random destination is found

    bool isFlying = false;
    bool isSwimming = true;
    switch (unit->GetTypeId())
    {
    case TYPEID_PLAYER:
        isFlying = static_cast<Player*>(unit)->IsFlying();
        break;
    case TYPEID_UNIT:
        isFlying = static_cast<Creature*>(unit)->IsFlying();
        isSwimming = static_cast<Creature*>(unit)->IsSwimming();
        break;
    default:
        sLog.outError("Map::GetReachableRandomPosition> Unsupported unit type is passed!");
        return false;
    }

    if (radius < 0.1f)
    {
        sLog.outError("Map::GetReachableRandomPosition> Unsupported unit type is passed!");
        return false;
    }

    if (isFlying)
    {
        newDestAssigned = GetRandomPointInTheAir(unit->GetPhaseMask(), i_x, i_y, i_z, radius);
        /*if (newDestAssigned)
        sLog.outString("Generating air random point for %s", GetGuidStr().c_str());*/
    }
    else
    {
        GridMapLiquidData liquid_status;
        GridMapLiquidStatus res = m_TerrainData->getLiquidStatus(i_x, i_y, i_z, MAP_ALL_LIQUIDS, &liquid_status);
        if (isSwimming && (res & (LIQUID_MAP_UNDER_WATER | LIQUID_MAP_IN_WATER)))
        {
            float water_z;
            if (GetTerrain()->IsUnderWater(i_x, i_y, i_z, &water_z))
                newDestAssigned = GetRandomPointUnderWater(unit->GetPhaseMask(), i_x, i_y, i_z, radius, water_z);
            /*if (newDestAssigned)
            sLog.outString("Generating swim random point for %s", GetGuidStr().c_str());*/
        }
        else
        {
            newDestAssigned = GetReachableRandomPointOnGround(unit->GetPhaseMask(), i_x, i_y, i_z, radius);
            /*if (newDestAssigned)
            sLog.outString("Generating ground random point for %s", GetGuidStr().c_str());*/
        }
    }

    if (newDestAssigned)
    {
        x = i_x;
        y = i_y;
        z = i_z;
        return true;
    }

    return false;
}

bool Map::SendToPlayersInZone(WorldPacket const* data, uint32 zoneId) const
{
    bool foundPlayer = false;
    for (MapRefManager::const_iterator itr = m_mapRefManager.begin(); itr != m_mapRefManager.end(); ++itr)
    {
        if (itr->getSource()->GetZoneId() == zoneId)
        {
            itr->getSource()->GetSession()->SendPacket(data);
            foundPlayer = true;
        }
    }
    return foundPlayer;
}
