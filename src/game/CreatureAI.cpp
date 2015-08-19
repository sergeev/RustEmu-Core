/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
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

#include "CreatureAI.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Spell.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"

static_assert(MAXIMAL_AI_EVENT_EVENTAI <= 32, "Maximal 32 AI_EVENTs supported with EventAI");

CreatureAI::~CreatureAI()
{
}

void CreatureAI::AttackedBy(Unit* attacker)
{
    if (!m_creature->getVictim())
        AttackStart(attacker);

    DEBUG_FILTER_LOG( LOG_FILTER_COMBAT, "Creature %s attacked by %s", 
                      m_creature->GetGuidStr( ).c_str( ),
                      attacker->GetGuidStr( ).c_str( ) );
    
    m_creature->SetCombatStartPosition( m_creature->GetPositionX( ),
                                        m_creature->GetPositionY( ),
                                        m_creature->GetPositionZ( ) );    
}

Unit* CreatureAI::SelectVictim()
{
    return m_creature->SelectHostileTarget() ? m_creature->getVictim() : NULL;
}

CanCastResult CreatureAI::CanCastSpell(Unit* pTarget, const SpellEntry* pSpell, bool isTriggered)
{
    if (!pTarget)
        return CAST_FAIL_OTHER;

    // If not triggered, we check
    if (!isTriggered)
    {
        // State does not allow
        if (m_creature->hasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL))
            return CAST_FAIL_STATE;

        if (pSpell->PreventionType == SPELL_PREVENTION_TYPE_SILENCE && m_creature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SILENCED))
            return CAST_FAIL_STATE;

        if (pSpell->PreventionType == SPELL_PREVENTION_TYPE_PACIFY && m_creature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PACIFIED))
            return CAST_FAIL_STATE;

        // Check for power (also done by Spell::CheckCast())
        if ((int32)m_creature->GetPower((Powers)pSpell->GetPowerType()) < Spell::CalculatePowerCost(pSpell, m_creature))
            return CAST_FAIL_POWER;

        if (m_creature->HasSpellCooldown(pSpell))
            return CAST_FAIL_SPELL_COOLDOWN;
    }

    if (SpellRangeEntry const* pSpellRange = sSpellRangeStore.LookupEntry(pSpell->GetRangeIndex()))
    {
        if (pTarget != m_creature)
        {
            // pTarget is out of range of this spell (also done by Spell::CheckCast())
            float fDistance = m_creature->GetCombatDistance(pTarget, pSpell->rangeIndex == SPELL_RANGE_IDX_COMBAT);

            if (fDistance > (m_creature->IsHostileTo(pTarget) ? pSpellRange->maxRange : pSpellRange->maxRangeFriendly))
                return CAST_FAIL_TOO_FAR;

            float fMinRange = m_creature->IsHostileTo(pTarget) ? pSpellRange->minRange : pSpellRange->minRangeFriendly;

            if (fMinRange && fDistance < fMinRange)
                return CAST_FAIL_TOO_CLOSE;
        }

        return CAST_OK;
    }
    else
        return CAST_FAIL_OTHER;
}

CanCastResult CreatureAI::DoCastSpellIfCan(Unit* pTarget, uint32 uiSpell, uint32 uiCastFlags, ObjectGuid OriginalCasterGuid)
{
    if (!pTarget)
        return CAST_FAIL_OTHER;

    Unit* pCaster = m_creature;

    if (uiCastFlags & CAST_FORCE_TARGET_SELF)
        pCaster = pTarget;

    // Allowed to cast only if not casting (unless we interrupt ourself) or if spell is triggered
    if (!pCaster->IsNonMeleeSpellCasted(false) || (uiCastFlags & (CAST_TRIGGERED | CAST_INTERRUPT_PREVIOUS)))
    {
        if (const SpellEntry* pSpell = sSpellStore.LookupEntry(uiSpell))
        {
            // If cast flag CAST_AURA_NOT_PRESENT is active, check if target already has aura on them
            if (uiCastFlags & CAST_AURA_NOT_PRESENT)
            {
                if (pTarget->HasAura(uiSpell))
                    return CAST_FAIL_TARGET_AURA;
            }

            // Check if cannot cast spell
            if (!(uiCastFlags & (CAST_FORCE_TARGET_SELF | CAST_FORCE_CAST)))
            {
                CanCastResult castResult = CanCastSpell(pTarget, pSpell, uiCastFlags & CAST_TRIGGERED);

                if (castResult != CAST_OK)
                    return castResult;
            }

            // Interrupt any previous spell
            if ((uiCastFlags & CAST_INTERRUPT_PREVIOUS) && pCaster->IsNonMeleeSpellCasted(false))
                pCaster->InterruptNonMeleeSpells(false);

            pCaster->CastSpell(pTarget, pSpell, uiCastFlags & CAST_TRIGGERED, NULL, NULL, OriginalCasterGuid);
            return CAST_OK;
        }
        else
        {
            sLog.outErrorDb("DoCastSpellIfCan by creature entry %u attempt to cast spell %u but spell does not exist.", m_creature->GetEntry(), uiSpell);
            return CAST_FAIL_OTHER;
        }
    }
    else
        return CAST_FAIL_IS_CASTING;
}

bool CreatureAI::DoMeleeAttackIfReady()
{
    return m_creature->UpdateMeleeAttackingState();
}

void CreatureAI::SetCombatMovement(bool enable, bool stopOrStartMovement /*=false*/)
{
    m_isCombatMovement = enable;

    if (enable)
        m_creature->clearUnitState(UNIT_STAT_NO_COMBAT_MOVEMENT);
    else
        m_creature->addUnitState(UNIT_STAT_NO_COMBAT_MOVEMENT);

    if (!stopOrStartMovement)
        return;

    if (Unit* pVictim = m_creature->getVictim()) // Only change current movement while in combat
    {
        if (enable)
            m_creature->GetMotionMaster()->MoveChase(pVictim, m_attackDistance, m_attackAngle);
        else if (m_creature->IsInUnitState(UNIT_ACTION_CHASE))
            m_creature->StopMoving();
    }
}

void CreatureAI::HandleMovementOnAttackStart(Unit* victim)
{
    if (m_isCombatMovement)
        m_creature->GetMotionMaster()->MoveChase(victim, m_attackDistance, m_attackAngle);
    else if (m_creature->IsInUnitState(UNIT_ACTION_CHASE))
    {
        m_creature->GetUnitStateMgr().DropAction(UNIT_ACTION_CHASE);
        m_creature->StopMoving();
    }
}

// ////////////////////////////////////////////////////////////////////////////////////////////////
//                                      Event system
// ////////////////////////////////////////////////////////////////////////////////////////////////

void CreatureAI::SendAIEventAround(AIEventType eventType, Unit* pInvoker, uint32 uiDelay, float fRadius, uint32 miscValue /*=0*/) const
{
    if (fRadius > 0)
    {
        std::list<Creature*> receiverList;

        // Allow sending custom AI events to all units in range
        if (eventType >= AI_EVENT_CUSTOM_EVENTAI_A && eventType <= AI_EVENT_CUSTOM_EVENTAI_F && eventType != AI_EVENT_GOT_CCED)
        {
            MaNGOS::AnyUnitInObjectRangeCheck u_check(m_creature, fRadius);
            MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(receiverList, u_check);
            Cell::VisitGridObjects(m_creature, searcher, fRadius);
        }
        else
        {
            // Use this check here to collect only assitable creatures in case of CALL_ASSISTANCE, else be less strict
            MaNGOS::AnyAssistCreatureInRangeCheck u_check(m_creature, eventType == AI_EVENT_CALL_ASSISTANCE ? pInvoker : NULL, fRadius);
            MaNGOS::CreatureListSearcher<MaNGOS::AnyAssistCreatureInRangeCheck> searcher(receiverList, u_check);
            Cell::VisitGridObjects(m_creature, searcher, fRadius);
        }

        if (!receiverList.empty())
        {
            AiDelayEventAround* e = new AiDelayEventAround(eventType, pInvoker ? pInvoker->GetObjectGuid() : ObjectGuid(), *m_creature, receiverList, miscValue);
            m_creature->AddEvent(e, uiDelay);
        }
    }
}

void CreatureAI::SendAIEvent(AIEventType eventType, Unit* pInvoker, Creature* pReceiver, uint32 miscValue /*=0*/) const
{
    MANGOS_ASSERT(pReceiver);
    pReceiver->AI()->ReceiveAIEvent(eventType, m_creature, pInvoker, miscValue);
}
