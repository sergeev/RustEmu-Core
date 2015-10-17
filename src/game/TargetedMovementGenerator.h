/*
 * Copyright (C) 2005-2012 MaNGOS <http://getmangos.com/>
 * Copyright (c) 2013 MangosR2 <http://github.com/MangosR2>
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

#ifndef MANGOS_TARGETEDMOVEMENTGENERATOR_H
#define MANGOS_TARGETEDMOVEMENTGENERATOR_H

#include "MovementGenerator.h"
#include "FollowerReference.h"
#include "PathFinder.h"
#include "Unit.h"

class MANGOS_DLL_SPEC TargetedMovementGeneratorBase
{
    public:
        TargetedMovementGeneratorBase(Unit &target) { m_target.link(&target, this); }
        void stopFollowing() {}
    protected:
        FollowerReference m_target;
};

template<class T, typename D>
class MANGOS_DLL_SPEC TargetedMovementGeneratorMedium : public MovementGeneratorMedium< T, D >, public TargetedMovementGeneratorBase
{
    protected:
        TargetedMovementGeneratorMedium(Unit& target, float offset, float angle) :
            TargetedMovementGeneratorBase(target),
            m_recheckDistanceTimer(0), m_waitTargetTimer(0),
            m_offset(offset), m_angle(angle),
            m_speedChanged(false), m_targetReached(false),
            m_path(nullptr) {}
        virtual ~TargetedMovementGeneratorMedium() { delete m_path; }

    public:

        bool IsReachable() const
        {
            return (m_path) ? (m_path->getPathType() & PATHFIND_NORMAL) : true;
        }

        Unit* GetTarget() const { return m_target.getTarget(); }

        void UnitSpeedChanged() { m_speedChanged = true; }
        char const* Name() const { return "<TargetedMedium>"; }

        bool update(T&, const uint32&);

    protected:
        void _setTargetLocation(T&, bool updateDestination);
        bool RequiresNewPosition(T& owner, float x, float y, float z);

        ShortTimeTracker m_recheckDistanceTimer;
        uint32 m_waitTargetTimer;
        float m_offset;
        float m_angle;
        bool m_speedChanged : 1;
        bool m_targetReached : 1;

        PathFinder* m_path;
};

template<class T>
class MANGOS_DLL_SPEC ChaseMovementGenerator : public TargetedMovementGeneratorMedium<T, ChaseMovementGenerator<T> >
{
    public:
        ChaseMovementGenerator(Unit& target)
            : TargetedMovementGeneratorMedium<T, ChaseMovementGenerator<T> >(target, 0.0f, 0.0f) {}
        ChaseMovementGenerator(Unit& target, float offset, float angle)
            : TargetedMovementGeneratorMedium<T, ChaseMovementGenerator<T> >(target, offset, angle) {}
        ~ChaseMovementGenerator() {}

        MovementGeneratorType GetMovementGeneratorType() const override { return CHASE_MOTION_TYPE; }

        char const* Name() const { return "<Chase>"; }

        static void _clearUnitStateMove(T& u) { u.clearUnitState(UNIT_STAT_CHASE_MOVE); }
        static void _addUnitStateMove(T& u)  { u.addUnitState(UNIT_STAT_CHASE_MOVE); }
        bool EnableWalking() const { return false;}
        bool _lostTarget(T& u) const { return u.getVictim() != this->GetTarget(); }
        void _reachTarget(T&);
        float GetDynamicTargetDistance(T& owner, bool forRangeCheck) const;

        void initialize(T&);
        void finalize(T&);
        void interrupt(T&);
        void reset(T&);
};

template<class T>
class MANGOS_DLL_SPEC FollowMovementGenerator : public TargetedMovementGeneratorMedium<T, FollowMovementGenerator<T> >
{
    public:
        FollowMovementGenerator(Unit& target)
            : TargetedMovementGeneratorMedium<T, FollowMovementGenerator<T> >(target) {}
        FollowMovementGenerator(Unit& target, float offset, float angle)
            : TargetedMovementGeneratorMedium<T, FollowMovementGenerator<T> >(target, offset, angle) {}
        ~FollowMovementGenerator() {}

        MovementGeneratorType GetMovementGeneratorType() const override { return FOLLOW_MOTION_TYPE; }

        char const* Name() const { return "<Follow>"; }

        static void _clearUnitStateMove(T& u) { u.clearUnitState(UNIT_STAT_FOLLOW_MOVE); }
        static void _addUnitStateMove(T& u)  { u.addUnitState(UNIT_STAT_FOLLOW_MOVE); }
        bool EnableWalking() const;
        bool _lostTarget(T&) const { return false; }
        void _reachTarget(T&);
        float GetDynamicTargetDistance(T& owner, bool forRangeCheck) const;

        void initialize(T&);
        void finalize(T&);
        void interrupt(T&);
        void reset(T&);

    private:
        void _updateSpeed(T& u);
};

#endif
