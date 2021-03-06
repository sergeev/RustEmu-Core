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

#include "ConfusedMovementGenerator.h"
#include "MapManager.h"
#include "Creature.h"
#include "Player.h"
#include "movement/MoveSplineInit.h"
#include "movement/MoveSpline.h"
#include "PathFinder.h"

template<class T>
void ConfusedMovementGenerator<T>::initialize(T& unit)
{
    unit.addUnitState(UNIT_STAT_CONFUSED);

    // set initial position
    unit.GetPosition(i_x, i_y, i_z);

    if (!unit.isAlive() || unit.hasUnitState(UNIT_STAT_NOT_MOVE))
        return;

    unit.StopMoving();
    unit.addUnitState(UNIT_STAT_CONFUSED_MOVE);
}

template<class T>
void ConfusedMovementGenerator<T>::interrupt(T& unit)
{
    unit.InterruptMoving();
    // confused state still applied while movegen disabled
    unit.clearUnitState(UNIT_STAT_CONFUSED | UNIT_STAT_CONFUSED_MOVE);
}

template<class T>
void ConfusedMovementGenerator<T>::reset(T& unit)
{
    i_nextMoveTime.Reset(0);

    if (!unit.isAlive() || unit.hasUnitState(UNIT_STAT_NOT_MOVE))
        return;

    unit.StopMoving();
    unit.addUnitState(UNIT_STAT_CONFUSED | UNIT_STAT_CONFUSED_MOVE);
}

template<class T>
bool ConfusedMovementGenerator<T>::update(T& unit, const uint32& diff)
{
    // ignore in case other no reaction state
    if (unit.hasUnitState(UNIT_STAT_CAN_NOT_REACT & ~UNIT_STAT_CONFUSED))
        return true;

    if (i_nextMoveTime.Passed())
    {
        // currently moving, update location
        unit.addUnitState(UNIT_STAT_CONFUSED_MOVE);

        if (unit.movespline->Finalized())
            i_nextMoveTime.Reset(urand(800, 1500));
    }
    else
    {
        // waiting for next move
        i_nextMoveTime.Update(diff);
        if (i_nextMoveTime.Passed())
        {
            // start moving
            unit.addUnitState(UNIT_STAT_CONFUSED_MOVE);

            float x, y, z;
            unit.GetNearPoint(&unit, x, y, z, unit.GetObjectBoundingRadius(), 10.0f, rand_norm_f() * M_PI_F * 2.0f);
            unit.UpdateAllowedPositionZ(x, y, z);

            PathFinder path(&unit);
            path.setPathLengthLimit(30.0f);
            path.calculate(x, y, z);
            if (path.getPathType() & PATHFIND_NOPATH)
            {
                i_nextMoveTime.Reset(urand(800, 1000));
                return true;
            }

            Movement::MoveSplineInit<Unit*> init(unit);
            init.MovebyPath(path.getPath());
            init.SetWalk(true);
            init.Launch();
        }
    }

    return true;
}

template<>
void ConfusedMovementGenerator<Player>::finalize(Player& unit)
{
    unit.StopMoving();
    unit.clearUnitState(UNIT_STAT_CONFUSED | UNIT_STAT_CONFUSED_MOVE);
}

template<>
void ConfusedMovementGenerator<Creature>::finalize(Creature& unit)
{
    unit.clearUnitState(UNIT_STAT_CONFUSED | UNIT_STAT_CONFUSED_MOVE);
}

template void ConfusedMovementGenerator<Player>::initialize(Player& player);
template void ConfusedMovementGenerator<Creature>::initialize(Creature& creature);
template void ConfusedMovementGenerator<Player>::interrupt(Player& player);
template void ConfusedMovementGenerator<Creature>::interrupt(Creature& creature);
template void ConfusedMovementGenerator<Player>::reset(Player& player);
template void ConfusedMovementGenerator<Creature>::reset(Creature& creature);
template bool ConfusedMovementGenerator<Player>::update(Player& player, const uint32& diff);
template bool ConfusedMovementGenerator<Creature>::update(Creature& creature, const uint32& diff);
