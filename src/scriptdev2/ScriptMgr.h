/* Copyright (C) 2006 - 2012 ScriptDev2 <http://www.scriptdev2.com/>
 * This program is free software licensed under GPL version 2
 * Please see the included DOCS/LICENSE.TXT for more information */

#ifndef SC_SCRIPTMGR_H
#define SC_SCRIPTMGR_H

#include "Common.h"
#include "DBCStructure.h"
#include "Database/DatabaseEnv.h"

class Player;
class Creature;
class CreatureAI;
class InstanceData;
class Quest;
class Item;
class GameObject;
class SpellCastTargets;
class Map;
class Unit;
class WorldObject;
class Aura;
class Object;
class ObjectGuid;

// *********************************************************
// ************** Some defines used globally ***************

// Basic defines
#define VISIBLE_RANGE       (166.0f)                        // MAX visible range (size of grid)
#define DEFAULT_TEXT        "<ScriptDev2 Text Entry Missing!>"

/* Escort Factions
 * TODO: find better namings and definitions.
 * N=Neutral, A=Alliance, H=Horde.
 * NEUTRAL or FRIEND = Hostility to player surroundings (not a good definition)
 * ACTIVE or PASSIVE = Hostility to environment surroundings.
 */
enum EscortFaction
{
    FACTION_ESCORT_A_NEUTRAL_PASSIVE    = 10,
    FACTION_ESCORT_H_NEUTRAL_PASSIVE    = 33,
    FACTION_ESCORT_N_NEUTRAL_PASSIVE    = 113,

    FACTION_ESCORT_A_NEUTRAL_ACTIVE     = 231,
    FACTION_ESCORT_H_NEUTRAL_ACTIVE     = 232,
    FACTION_ESCORT_N_NEUTRAL_ACTIVE     = 250,

    FACTION_ESCORT_N_FRIEND_PASSIVE     = 290,
    FACTION_ESCORT_N_FRIEND_ACTIVE      = 495,

    FACTION_ESCORT_A_PASSIVE            = 774,
    FACTION_ESCORT_H_PASSIVE            = 775,

    FACTION_ESCORT_N_ACTIVE             = 1986,
    FACTION_ESCORT_H_ACTIVE             = 2046
};

// *********************************************************
// ************* Some structures used globally *************

struct Script
{
    Script(char const* scriptName = nullptr) : Name(scriptName),
        pGossipHello(nullptr), pGossipHelloGO(nullptr), pGossipSelect(nullptr), pGossipSelectGO(nullptr),
        pGossipSelectWithCode(nullptr), pGossipSelectGOWithCode(nullptr),
        pDialogStatusNPC(nullptr), pDialogStatusGO(nullptr),
        pQuestAcceptNPC(nullptr), pQuestAcceptGO(nullptr), pQuestAcceptItem(nullptr),
        pQuestRewardedNPC(nullptr), pQuestRewardedGO(nullptr),
        pGOUse(nullptr), pItemUse(nullptr), pAreaTrigger(nullptr), pNpcSpellClick(nullptr), pProcessEventId(nullptr),
        pEffectDummyNPC(nullptr), pEffectDummyGO(nullptr), pEffectDummyItem(nullptr), pEffectScriptEffectNPC(nullptr),
        pEffectAuraDummy(nullptr), GetAI(nullptr), GetInstanceData(nullptr)
    {}

    char const* Name;

    bool (*pGossipHello             )(Player*, Creature*);
    bool (*pGossipHelloGO           )(Player*, GameObject*);
    bool (*pGossipSelect            )(Player*, Creature*, uint32, uint32);
    bool (*pGossipSelectGO          )(Player*, GameObject*, uint32, uint32);
    bool (*pGossipSelectWithCode    )(Player*, Creature*, uint32, uint32, const char*);
    bool (*pGossipSelectGOWithCode  )(Player*, GameObject*, uint32, uint32, const char*);
    uint32 (*pDialogStatusNPC       )(Player*, Creature*);
    uint32 (*pDialogStatusGO        )(Player*, GameObject*);
    bool (*pQuestAcceptNPC          )(Player*, Creature*, Quest const*);
    bool (*pQuestAcceptGO           )(Player*, GameObject*, Quest const*);
    bool (*pQuestAcceptItem         )(Player*, Item*, Quest const*);
    bool (*pQuestRewardedNPC        )(Player*, Creature*, Quest const*);
    bool (*pQuestRewardedGO         )(Player*, GameObject*, Quest const*);
    bool (*pGOUse                   )(Player*, GameObject*);
    bool (*pItemUse                 )(Player*, Item*, SpellCastTargets const&);
    bool (*pAreaTrigger             )(Player*, AreaTriggerEntry const*);
    bool (*pNpcSpellClick           )(Player*, Creature*, uint32);
    bool (*pProcessEventId          )(uint32, Object*, Object*, bool);
    bool (*pEffectDummyNPC          )(Unit*, uint32, SpellEffectIndex, Creature*, ObjectGuid);
    bool (*pEffectDummyGO           )(Unit*, uint32, SpellEffectIndex, GameObject*, ObjectGuid);
    bool (*pEffectDummyItem         )(Unit*, uint32, SpellEffectIndex, Item*, ObjectGuid);
    bool (*pEffectScriptEffectNPC   )(Unit*, uint32, SpellEffectIndex, Creature*, ObjectGuid);
    bool (*pEffectAuraDummy         )(const Aura*, bool);

    CreatureAI* (*GetAI             )(Creature*);
    InstanceData* (*GetInstanceData )(Map*);

    void RegisterSelf(bool bReportError = true);
};

// *********************************************************
// ******************* AutoScript **************************

typedef CreatureAI* (*TGetAI)(Creature*);
typedef InstanceData* (*TGetInstanceData)(Map*);

class AutoScript
{
    private:
        Script* m_script;
        bool m_reportError;

        void Register();

    public:
        AutoScript() : m_script(nullptr), m_reportError(true) {}
        AutoScript(char const* scriptName, bool reportError = true) : m_script(nullptr) { newScript(scriptName, reportError); }
        AutoScript(char const* scriptName, TGetAI getAIPtr, bool reportError = true) : m_script(nullptr) { newScript(scriptName, reportError); m_script->GetAI = getAIPtr; }
        AutoScript(char const* scriptName, TGetInstanceData getIDPtr, bool reportError = true) : m_script(nullptr) { newScript(scriptName, reportError); m_script->GetInstanceData = getIDPtr; }

        ~AutoScript() { Register(); }

        Script* newScript(char const* scriptName, bool reportError = true);
        Script* newScript(char const* scriptName, TGetAI getAIPtr, bool reportError = true);

        Script* operator -> ()
        {
            MANGOS_ASSERT(m_script != nullptr && "AutoScript: use newScript() before!");
            return m_script;
        }
};

// *********************************************************
// ************* Some functions used globally **************

// Generic scripting text function
void DoScriptText(int32 iTextEntry, WorldObject* pSource, Unit* pTarget = nullptr);
void DoOrSimulateScriptTextForMap(int32 iTextEntry, uint32 uiCreatureEntry, Map* pMap, Creature* pCreatureSource = nullptr, Unit* pTarget = nullptr);

//DB query
QueryResult* strSD2Pquery(char*);

// Not registered scripts storage
Script* GetScriptByName(std::string scriptName);

// *********************************************************
// **************** Internal hook mechanics ****************

#if COMPILER == COMPILER_GNU
#define FUNC_PTR(name,callconvention,returntype,parameters)    typedef returntype(*name)parameters __attribute__ ((callconvention));
#else
#define FUNC_PTR(name, callconvention, returntype, parameters)    typedef returntype(callconvention *name)parameters;
#endif

#endif
