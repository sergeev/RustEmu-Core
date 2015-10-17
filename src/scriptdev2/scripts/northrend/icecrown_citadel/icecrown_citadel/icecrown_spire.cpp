/* Copyright (C) 2006 - 2013 ScriptDev2 <http://www.scriptdev2.com/>
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

/* ScriptData
SDName: icecrown_spire
SD%Complete: 100%
SDComment: by /dev/rsa
SDCategory: Icecrown Citadel - mobs
EndScriptData */

#include "precompiled.h"
#include "icecrown_citadel.h"
enum
{
        SPELL_BERSERK                           = 47008,
        SPELL_FROST_BREATH                      = 70116,
        SPELL_BLIZZARD                          = 70362,
        SPELL_CLEAVE                            = 70361,

        SPELL_STOMP                             = 64652,
        SPELL_DEATH_PLAGUE                      = 72865,
//        SPELL_DEATH_PLAGUE                      = 72879,

        SPELL_ACHIEVEMENT_CREDIT                = 72959,

};

struct mob_spire_frostwyrmAI : public BSWScriptedAI
{
    mob_spire_frostwyrmAI(Creature* pCreature) : BSWScriptedAI(pCreature)
    {
        pInstance = (ScriptedInstance*)pCreature->GetInstanceData();
        Reset();
    }

    ScriptedInstance* pInstance;
    uint8 m_uiStage;

    void Reset() override
    {
        m_creature->SetRespawnDelay(DAY);
        m_uiStage = 0;
        resetTimers();
    }

    void UpdateAI(const uint32 uiDiff) override
    {
        if (!m_creature->SelectHostileTarget() || !m_creature->getVictim())
            return;

        switch (m_uiStage)
        {
            case 0:
                    break;
            case 1:
                    doCast(SPELL_BERSERK);
                    m_uiStage = 2;
                    break;
            case 2:
            default:
                    break;
        }

        timedCast(SPELL_CLEAVE, uiDiff);
        timedCast(SPELL_BLIZZARD, uiDiff);
        timedCast(SPELL_FROST_BREATH, uiDiff);

        if (m_creature->GetHealthPercent() < 10.0f && m_uiStage == 0)
            m_uiStage = 1;

        timedCast(SPELL_BERSERK, uiDiff);

        DoMeleeAttackIfReady();

    }
};

CreatureAI* GetAI_mob_spire_frostwyrm(Creature* pCreature)
{
    return new mob_spire_frostwyrmAI(pCreature);
}

struct mob_frost_giantAI : public BSWScriptedAI
{
    mob_frost_giantAI(Creature* pCreature) : BSWScriptedAI(pCreature)
    {
        pInstance = (ScriptedInstance*)pCreature->GetInstanceData();
        Reset();
    }

    ScriptedInstance* pInstance;
    uint8 m_uiStage;

    void Aggro(Unit* /*pWho*/) override
    {
        if (pInstance)
            pInstance->SetData(TYPE_FLIGHT_WAR, IN_PROGRESS);
    }

    void JustDied(Unit* pKiller) override
    {
        if (!pInstance)
            return;
        if (pKiller->GetTypeId() == TYPEID_PLAYER || pKiller->GetCharmerOrOwnerOrSelf()->GetTypeId() == TYPEID_PLAYER )
              pInstance->SetData(TYPE_FLIGHT_WAR, DONE);

        // Temporary
        // Untill Gunship Battle will be implemented
        m_creature->CastSpell(m_creature, SPELL_ACHIEVEMENT_CREDIT, false);
    }

    void JustReachedHome() override
    {
        if (pInstance) pInstance->SetData(TYPE_FLIGHT_WAR, FAIL);
    }

    void Reset() override
    {
        m_creature->SetRespawnDelay(7*DAY);
        m_uiStage = 0;
        resetTimers();
    }

    void UpdateAI(const uint32 uiDiff) override
    {
        if (!m_creature->SelectHostileTarget() || !m_creature->getVictim())
            return;

        switch (m_uiStage)
        {
            case 0:
                    break;
            case 1:
                    doCast(SPELL_BERSERK);
                    m_uiStage = 2;
                    break;
            case 2:
            default:
                    break;
        }
        timedCast(SPELL_STOMP, uiDiff);
        timedCast(SPELL_DEATH_PLAGUE, uiDiff);

        if (m_creature->GetHealthPercent() < 2.0f && m_uiStage == 0)
            m_uiStage = 1;

        timedCast(SPELL_BERSERK, uiDiff);

        DoMeleeAttackIfReady();

    }
};

CreatureAI* GetAI_mob_frost_giant(Creature* pCreature)
{
    return new mob_frost_giantAI(pCreature);
}

void AddSC_icecrown_spire()
{
    Script* pNewScript;

    pNewScript = new Script;
    pNewScript->Name = "mob_spire_frostwyrm";
    pNewScript->GetAI = &GetAI_mob_spire_frostwyrm;
    pNewScript->RegisterSelf();

    pNewScript = new Script;
    pNewScript->Name = "mob_frost_giant";
    pNewScript->GetAI = &GetAI_mob_frost_giant;
    pNewScript->RegisterSelf();
}
