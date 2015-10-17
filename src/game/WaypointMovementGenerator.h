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

#ifndef MANGOS_WAYPOINTMOVEMENTGENERATOR_H
#define MANGOS_WAYPOINTMOVEMENTGENERATOR_H

/** @page PathMovementGenerator is used to generate movements
 * of waypoints and flight paths.  Each serves the purpose
 * of generate activities so that it generates updated
 * packets for the players.
 */

#include "MovementGenerator.h"
#include "WaypointManager.h"
#include "DBCStructure.h"
#include "Player.h"

#include <vector>
#include <set>

#define FLIGHT_TRAVEL_UPDATE  100
#define STOP_TIME_FOR_PLAYER  (3 * MINUTE * IN_MILLISECONDS)// 3 Minutes

class GameObject;

template<class T, class P>
class MANGOS_DLL_SPEC PathMovementBase
{
    public:
        PathMovementBase() : m_path(nullptr), m_currentNode(0) {}
        virtual ~PathMovementBase() {}

        // template pattern, not defined .. override required
        void LoadPath(T&);
        uint32 GetCurrentNode() const { return m_currentNode; }

    protected:
        P m_path;
        uint32 m_currentNode;
};

/** WaypointMovementGenerator loads a series of way points
 * from the DB and apply it to the creature's movement generator.
 * Hence, the creature will move according to its predefined way points.
 */

template<class T>
class MANGOS_DLL_SPEC WaypointMovementGenerator;

template<>
class MANGOS_DLL_SPEC WaypointMovementGenerator<Creature> :
    public MovementGeneratorMedium<Creature, WaypointMovementGenerator<Creature> >,
    public PathMovementBase<Creature, WaypointPath const*>
{
    public:
        WaypointMovementGenerator(Creature&) : m_nextMoveTime(0), m_isArrivalDone(false), m_lastReachedWaypoint(0) {}
        ~WaypointMovementGenerator() { m_path = nullptr; }

        void InitializeWaypointPath(Creature& u, int32 id, WaypointPathOrigin wpSource, uint32 initialDelay, uint32 overwriteEntry);

        MovementGeneratorType GetMovementGeneratorType() const { return WAYPOINT_MOTION_TYPE; }
        uint32 getLastReachedWaypoint() const { return m_lastReachedWaypoint; }
        void GetPathInformation(int32& pathId, WaypointPathOrigin& wpOrigin) const { pathId = m_pathId; wpOrigin = m_PathOrigin; }
        void GetPathInformation(std::ostringstream& oss) const;

        void AddToWaypointPauseTime(int32 waitTimeDiff);
        bool SetNextWaypoint(uint32 pointId);

        void initialize(Creature& u);
        void interrupt(Creature&);
        void finalize(Creature&);
        void reset(Creature& u);
        bool update(Creature& u, const uint32& diff);
        bool getResetPosition(Creature&, float& /*x*/, float& /*y*/, 
                              float& /*z*/, float& /*o*/) const;

private:
    void LoadPath(Creature& c, int32 id, WaypointPathOrigin wpOrigin, uint32 overwriteEntry);

    void Stop(int32 time) { m_nextMoveTime.Reset(time); }
    bool Stopped(Creature& u);
    bool CanMove(int32 diff, Creature& u);

    void OnArrived(Creature&);
    void StartMove(Creature&);

    ShortTimeTracker m_nextMoveTime;
    bool m_isArrivalDone;
    uint32 m_lastReachedWaypoint;

    int32 m_pathId;
    WaypointPathOrigin m_PathOrigin;
};

/** FlightPathMovementGenerator generates movement of the player for the paths
 * and hence generates ground and activities for the player.
 */
class MANGOS_DLL_SPEC FlightPathMovementGenerator :
    public MovementGeneratorMedium<Player, FlightPathMovementGenerator >,
    public PathMovementBase<Player, TaxiPathNodeList const*>
{
    public:
        explicit FlightPathMovementGenerator(TaxiPathNodeList const& pathnodes, uint32 startNode = 0)
        {
            m_path = &pathnodes;
            m_currentNode = startNode;
        }

        MovementGeneratorType GetMovementGeneratorType() const { return FLIGHT_MOTION_TYPE; }

        TaxiPathNodeList const& GetPath() { return *m_path; }
        uint32 GetPathId() const { return (*m_path)[0].path; }
        uint32 GetPathAtMapEnd() const;
        bool HasArrived() const { return (m_currentNode >= m_path->size()); }
        void SetCurrentNodeAfterTeleport();
        void SkipCurrentNode() { ++m_currentNode; }
        void DoEventIfAny(Player& player, TaxiPathNodeEntry const& node, bool departure);

        virtual void initialize(Player &);
        virtual void finalize(Player &);
        virtual void interrupt(Player &);
        virtual void reset(Player &);
        virtual bool update(Player &, const uint32 &);
        virtual bool getResetPosition(Player&, float& x, float& y, float& z, float& o) const;

};

/** TransportPathMovementGenerator generates movement of the MO_TRANSPORT and elevators for the paths
 * and hence generates ground and activities.
 */
class MANGOS_DLL_SPEC TransportPathMovementGenerator :
    public PathMovementBase<GameObject,TaxiPathNodeList const*>
{
    public:
        explicit TransportPathMovementGenerator(TaxiPathNodeList const& pathnodes, uint32 startNode = 0)
        {
            m_path = &pathnodes;
            m_currentNode = startNode;
        }
        virtual void Initialize(GameObject &go);
        virtual void Finalize(GameObject &go);
        virtual void Interrupt(GameObject &go);
        virtual void Reset(GameObject &go);

        bool Update(GameObject&, const uint32&);
        MovementGeneratorType GetMovementGeneratorType() const { return FLIGHT_MOTION_TYPE; }

        TaxiPathNodeList const& GetPath() { return *m_path; }
        uint32 GetPathAtMapEnd() const;
        bool HasArrived() const { return (m_currentNode >= m_path->size()); }
        void SetCurrentNodeAfterTeleport();
        void SkipCurrentNode() { ++m_currentNode; }
        void DoEventIfAny(GameObject& go, TaxiPathNodeEntry const& node, bool departure);
        bool GetResetPosition(GameObject& go, float& x, float& y, float& z) const;
};

#endif
