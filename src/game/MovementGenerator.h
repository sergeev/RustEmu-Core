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

#ifndef MANGOS_MOVEMENTGENERATOR_H
#define MANGOS_MOVEMENTGENERATOR_H

#include "Common.h"
#include "Platform/Define.h"
#include "Policies/Singleton.h"
#include "Dynamic/ObjectRegistry.h"
#include "Dynamic/FactoryHolder.h"
#include "MotionMaster.h"
#include "StateMgr.h"
#include "Timer.h"

class Unit;
class Creature;
class Player;

class MANGOS_DLL_SPEC MovementGenerator : public UnitAction
{
    public:
        virtual ~MovementGenerator();

        // called before adding movement generator to motion stack
        virtual void Initialize(Unit &) = 0;
        // called aftre remove movement generator from motion stack
        virtual void Finalize(Unit &) = 0;

        // called before lost top position (before push new movement generator above)
        virtual void Interrupt(Unit &) = 0;
        // called after return movement generator to top position (after remove above movement generator)
        virtual void Reset(Unit &) = 0;

        virtual bool Update(Unit &, const uint32 &time_diff) = 0;

        virtual MovementGeneratorType GetMovementGeneratorType() const = 0;

        virtual void UnitSpeedChanged() { }

        // used by Evade code for select point to evade with expected restart default movement
        virtual bool GetResetPosition(Unit&, float& /*x*/, float& /*y*/, float& /*z*/, float& /*o*/) const { return false; }

        // given destination unreachable? due to pathfinsing or other
        virtual bool IsReachable() const { return true; }

        // used for check from Update call is movegen still be active (top movement generator)
        // after some not safe for this calls
        bool IsActive(Unit& u);
};

template<class T, class D>
class MANGOS_DLL_SPEC MovementGeneratorMedium : public MovementGenerator
{
    public:
        void Initialize(Unit &u)
        {
          T *unit = dynamic_cast< T * >( &u );
          if ( unit != NULL ) ((D *)this)->initialize(*unit);
        }
        void Finalize(Unit &u)
        {
          T *unit = dynamic_cast< T * >( &u );
          if ( unit != NULL ) ((D *)this)->finalize(*unit);
        }
        void Interrupt(Unit &u)
        {
          T *unit = dynamic_cast< T * >( &u );
          if ( unit != NULL ) ((D *)this)->interrupt(*unit);
        }
        void Reset(Unit &u)
        {
          T *unit = dynamic_cast< T * >( &u );
          if ( unit != NULL ) ((D *)this)->reset(*unit);
        }
        bool Update(Unit &u, const uint32 &time_diff)
        {
          T *unit = dynamic_cast< T * >( &u );
          return ( unit != NULL ) ? ((D *)this)->update(*unit, time_diff) : true;
        }

        bool GetResetPosition(Unit& u, float& x, float& y, float& z, float& o) const override
        {
          T *unit = dynamic_cast< T * >( &u );
          return ( unit != NULL ) ? ((D *)this)->getResetPosition(*unit, x, y, z, o) : false;
        }

        // will not link if not overridden in the generators
        void initialize(T &u);
        void finalize(T &u);
        void interrupt(T &u);
        void reset(T &u);
        bool update(T &u, const uint32 &time_diff);

        // not need always overwrites
        bool getResetPosition(T& /*u*/, float& /*x*/, float& /*y*/, float& /*z*/, 
                              float& /*o*/) const { return false; }
};

struct SelectableMovement : public FactoryHolder<MovementGenerator,MovementGeneratorType>
{
    SelectableMovement(MovementGeneratorType mgt) : FactoryHolder<MovementGenerator,MovementGeneratorType>(mgt) {}
};

template<class REAL_MOVEMENT>
struct MovementGeneratorFactory : public SelectableMovement
{
    MovementGeneratorFactory(MovementGeneratorType mgt) : SelectableMovement(mgt) {}

    MovementGenerator* Create(void *) const;
};

typedef FactoryHolder<MovementGenerator,MovementGeneratorType> MovementGeneratorCreator;
typedef FactoryHolder<MovementGenerator,MovementGeneratorType>::FactoryHolderRegistry MovementGeneratorRegistry;
typedef FactoryHolder<MovementGenerator,MovementGeneratorType>::FactoryHolderRepository MovementGeneratorRepository;

#endif
