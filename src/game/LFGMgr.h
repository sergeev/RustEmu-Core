/*
 * Copyright (C) 2011-2012 /dev/rsa for MangosR2 <http://github.com/MangosR2>
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

#ifndef _LFGMGR_H
#define _LFGMGR_H

#include "Common.h"
#include "ObjectGuid.h"
#include "Policies/Singleton.h"
#include <boost/thread/mutex.hpp>
#include "LFG.h"
#include "Timer.h"

enum LFGenum
{
    LFG_TIME_ROLECHECK         = 2*MINUTE,
    LFG_TIME_BOOT              = 2*MINUTE,
    LFG_TIME_PROPOSAL          = 2*MINUTE,
    LFG_TIME_JOIN_WARNING      = 1*IN_MILLISECONDS,
    LFG_TANKS_NEEDED           = 1,
    LFG_HEALERS_NEEDED         = 1,
    LFG_DPS_NEEDED             = 3,
    LFG_QUEUEUPDATE_INTERVAL   = 35*IN_MILLISECONDS,
    LFG_UPDATE_INTERVAL        = 1*IN_MILLISECONDS,
    LFR_UPDATE_INTERVAL        = 3*IN_MILLISECONDS,
    DEFAULT_LFG_DELAY          = 1,
    SHORT_LFG_DELAY            = 3,
    LONG_LFG_DELAY             = 10,
    LFG_SPELL_DUNGEON_COOLDOWN = 71328,
    LFG_SPELL_DUNGEON_DESERTER = 71041,
    LFG_SPELL_LUCK_OF_THE_DRAW = 72221,
};

enum LFGEventType
{
    LFG_EVENT_NONE                        = 0,
    LFG_EVENT_TELEPORT_PLAYER             = 1,
    LFG_EVENT_TELEPORT_GROUP              = 2,
};

class Group;
class Player;
class Map;
struct LFGDungeonExpansionEntry;
struct LFGProposal;

// Reward info
struct LFGReward
{
    uint32 maxLevel;
    struct
    {
        uint32 questId;
        uint32 variableMoney;
        uint32 variableXP;
    } reward[2];

    LFGReward(uint32 _maxLevel, uint32 firstQuest, uint32 firstVarMoney, uint32 firstVarXp, uint32 otherQuest, uint32 otherVarMoney, uint32 otherVarXp)
        : maxLevel(_maxLevel)
    {
        reward[0].questId = firstQuest;
        reward[0].variableMoney = firstVarMoney;
        reward[0].variableXP = firstVarXp;
        reward[1].questId = otherQuest;
        reward[1].variableMoney = otherVarMoney;
        reward[1].variableXP = otherVarXp;
    }
};

// Stores player or group queue info
struct LFGQueueInfo
{
    LFGQueueInfo(ObjectGuid _guid, LFGType type, uint32 _queueID);
    ObjectGuid const guid;                                  // guid (player or group) of queue owner
    time_t     joinTime;                                    // Player/group queue join time (to calculate wait times)
    uint8      tanks;                                       // Tanks needed
    uint8      healers;                                     // Healers needed
    uint8      dps;                                         // Dps needed
    uint32 const queueID;                                   // Queue id

    // helpers
    LFGType const& GetDungeonType() const {return m_type;};
    private:
    LFGType const   m_type;
};

struct LFGQueueStatus
{
    LFGQueueStatus(): avgWaitTime(0), waitTime(0), waitTimeTanks(0), waitTimeDps(0),
                      tanks(0), healers(0), dps(0), playersWaited(0) {};

    time_t                 avgWaitTime;                      // Average Wait time
    time_t                 waitTime;                         // Wait Time
    time_t                 waitTimeTanks;                    // Wait Tanks
    time_t                 waitTimeHealer;                   // Wait Healers
    time_t                 waitTimeDps;                      // Wait Dps
    uint32                 tanks;                            // Tanks
    uint32                 healers;                          // Healers
    uint32                 dps;                              // Dps
    uint32                 playersWaited;                    // players summ
};

// Event manager
struct LFGEvent
{
    LFGEvent(LFGEventType _type, ObjectGuid _guid, uint8 _parm) :
            type(_type), guid(_guid), eventParm(_parm), executeTime(0) {};
    LFGEventType  type;
    ObjectGuid    guid;
    uint8         eventParm;
    time_t        executeTime;
    // helpers
    void          Start(uint32 delay) { executeTime = time_t(time(nullptr) + delay); };
    bool          IsActive() { return ((executeTime > 0) && (time_t(time(nullptr)) >= executeTime)); };
};

typedef UNORDERED_MULTIMAP<uint32 /*dungeonId*/, LFGReward /*reward*/> LFGRewardMap;
typedef std::pair<LFGRewardMap::const_iterator, LFGRewardMap::const_iterator> LFGRewardMapBounds;
typedef UNORDERED_MAP<ObjectGuid /*group or player guid*/, LFGQueueInfo> LFGQueueInfoMap;
typedef UNORDERED_MAP<uint32/*dungeonID*/, LFGDungeonEntry const*> LFGDungeonMap;
typedef UNORDERED_MAP<uint32/*ID*/, LFGProposal> LFGProposalMap;

typedef UNORDERED_MAP<LFGDungeonEntry const*, LFGQueueStatus> LFGQueueStatusMap;
typedef UNORDERED_MAP<ObjectGuid, LFGRoleMask>  LFGRolesMap;
typedef UNORDERED_SET<LFGQueueInfo*> LFGQueue;
typedef UNORDERED_MAP<LFGDungeonEntry const*, GuidSet> LFGSearchMap;
typedef std::list<LFGEvent> LFGEventList;

typedef UNORDERED_MAP<ObjectGuid /*group or player guid*/, LFGStateStructure*> LFGStatesMap;

class LFGMgr : public MaNGOS::Singleton<LFGMgr, MaNGOS::ClassLevelLockable<LFGMgr, boost::mutex> >
{
    public:
        LFGMgr();
        ~LFGMgr();

        // Update system
        void Update(uint32 diff);

        void TryCompleteGroups(LFGType type);
        bool TryAddMembersToGroup(Group* pGroup, GuidSet* players);
        void CompleteGroup(Group* pGroup, GuidSet* players);
        bool TryCreateGroup(LFGType type);

        // Join system
        void Join(Player* pPlayer);
        void Leave(Player* pPlayer);
        void Leave(Group* pGroup);
        void ClearLFRList(Player* pPlayer);

        // Queue system
        void AddToQueue(ObjectGuid guid, LFGType type, bool inBegin = false);
        void RemoveFromQueue(ObjectGuid guid);
        LFGQueueInfo* GetQueueInfo(ObjectGuid guid);
        GuidSet GetDungeonPlayerQueue(LFGDungeonEntry const* pDungeon, Team team = TEAM_NONE);
        GuidSet GetDungeonGroupQueue(LFGDungeonEntry const* pDungeon, Team team = TEAM_NONE);
        GuidSet GetDungeonPlayerQueue(LFGType type);
        GuidSet GetDungeonGroupQueue(LFGType type);

        // reward system
        void LoadRewards();
        LFGReward const* GetRandomDungeonReward(LFGDungeonEntry const* pDungeon, Player* pPlayer);
        void SendLFGRewards(Group* pGroup);
        void SendLFGReward(Player* pPlayer, LFGDungeonEntry const* pDungeon);
        void DungeonEncounterReached(Group* pGroup);

        // Proposal system
        uint32 CreateProposal(LFGDungeonEntry const* pDungeon, Group* pGroup = nullptr, GuidSet* playerGuids = nullptr);
        bool SendProposal(uint32 uiID, ObjectGuid guid);
        LFGProposal* GetProposal(uint32 uiID);
        void RemoveProposal(Player* pDecliner, uint32 uiID);
        void RemoveProposal(uint32 uiID, bool bSuccess = false);
        void UpdateProposal(uint32 uiID, ObjectGuid guid, bool bAccept);
        void CleanupProposals(LFGType type);
        Player* LeaderElection(GuidSet* playerGuids);

        // boot vote system
        void OfferContinue(Group* pGroup);
        void InitBoot(Player* pKicker, ObjectGuid victimGuid, std::string reason);
        void UpdateBoot(Player* pPlayer, LFGAnswer answer);
        void CleanupBoots(LFGType type);

        // teleport system
        void Teleport(Group* pGroup, bool bOut = false);
        void Teleport(Player* pPlayer, bool bOut = false, bool bFromOpcode = false);

        // LFR extend system
        void UpdateLFRGroups();
        bool IsGroupCompleted(Group* pGroup, uint8 uiAddMembers = 0);

        // Statistic system
        LFGQueueStatus* GetDungeonQueueStatus(LFGType type);
        void UpdateQueueStatus(Player* pPlayer);
        void UpdateQueueStatus(Group* pGroup);
        void UpdateQueueStatus(LFGType type);
        void SendStatistic(LFGType type);
        LFGQueueStatus* GetOverallQueueStatus();

        // Role check system
        void CleanupRoleChecks(LFGType type);
        void StartRoleCheck(Group* pGroup);
        void UpdateRoleCheck(Group* pGroup);
        bool CheckRoles(Group* pGroup, Player* pPlayer = nullptr);
        bool CheckRoles(LFGRolesMap& roleMap);
        bool RoleChanged(Player* pPlayer, LFGRoleMask roles);
        void SetGroupRoles(Group* pGroup, GuidSet* = nullptr);
        bool SetRoles(LFGRolesMap& roleMap);

        // Social check system
        bool HasIgnoreState(ObjectGuid guid1, ObjectGuid guid2);
        bool HasIgnoreState(Group* pGroup, ObjectGuid guid);
        bool CheckTeam(Group* pGroup, Player* pPlayer = nullptr);
        bool CheckTeam(ObjectGuid guid1, ObjectGuid guid2);

        // Dungeon operations
        LFGDungeonEntry const* GetDungeon(uint32 dungeonID);
        LFGDungeonSet GetRandomDungeonsForPlayer(Player* pPlayer);
        static bool IsRandomDungeon(LFGDungeonEntry const* dungeon);
        static bool CheckWorldEvent(LFGDungeonEntry const* dungeon);

        // Group operations
        void AddMemberToLFDGroup(ObjectGuid guid);
        void RemoveMemberFromLFDGroup(Group* pGroup, ObjectGuid guid);
        void LoadLFDGroupPropertiesForPlayer(Player* pPlayer);

        // Dungeon expand operations
        LFGDungeonSet ExpandRandomDungeonsForGroup(LFGDungeonEntry const* randomDungeon, GuidSet playerGuids);
        LFGDungeonEntry const* SelectRandomDungeonFromList(LFGDungeonSet dugeons);

        // Checks
        LFGJoinResult GetPlayerJoinResult(Player* pPlayer);
        LFGJoinResult GetGroupJoinResult(Group* pGroup);

        // Player status
        LFGLockStatusType GetPlayerLockStatus(Player* pPlayer, LFGDungeonEntry const* pDungeon);
        LFGLockStatusType GetPlayerExpansionLockStatus(Player* pPlayer, LFGDungeonEntry const* pDungeon);
        LFGLockStatusType GetGroupLockStatus(Group* pGroup, LFGDungeonEntry const* pDungeon);
        LFGLockStatusMap GetPlayerLockMap(Player* pPlayer);
        LFGLockStatusMap GetPlayerLockMap(ObjectGuid const& guid);

        // Search matrix
        void AddToSearchMatrix(ObjectGuid guid, bool inBegin = false);
        void RemoveFromSearchMatrix(ObjectGuid guid);
        GuidSet* GetPlayersForDungeon(LFGDungeonEntry const* pDungeon);
        bool IsInSearchFor(LFGDungeonEntry const* pDungeon, ObjectGuid guid);
        void CleanupSearchMatrix();

        // LFG states operations
        LFGPlayerState* GetLFGPlayerState(ObjectGuid const& guid);
        LFGGroupState*  GetLFGGroupState(ObjectGuid const& guid);
        LFGStateStructure* CreateLFGState(ObjectGuid const& guid);
        void RemoveLFGState(ObjectGuid const& guid);

        // Sheduler
        void SheduleEvent();
        void AddEvent(ObjectGuid guid, LFGEventType type, time_t delay = DEFAULT_LFG_DELAY, uint8 param = 0);

        // Scripts
        void OnPlayerEnterMap(Player* pPlayer, Map* pMap);
        void OnPlayerLeaveMap(Player* pPlayer, Map* pMap);

    private:
        uint32 GenerateProposalID();
        uint32          m_proposalID;
        uint32 GenerateQueueID();
        uint32          m_queueID;

        LFGRewardMap    m_RewardMap;                        // Stores rewards for random dungeons
        LFGQueueInfoMap m_queueInfoMap;                     // Storage for queues
        LFGQueue        m_playerQueue[LFG_TYPE_MAX];        // Queue's for players
        LFGQueue        m_groupQueue[LFG_TYPE_MAX];         // Queue's for groups
        LFGDungeonMap   m_dungeonMap;                       // sorted dungeon map
        LFGQueueStatus  m_queueStatus[LFG_TYPE_MAX];        // Queue statisic
        LFGProposalMap  m_proposalMap;                      // Proposal store
        LFGSearchMap    m_searchMatrix;                     // Search matrix
        LFGEventList    m_eventList;                        // events storage
        LFGStatesMap    m_statesMap;                        // LFG states storage (for players or groups)

        IntervalTimer   m_LFGupdateTimer;                   // update timer for cleanup/statistic
        IntervalTimer   m_LFRupdateTimer;                   // update timer for LFR extend system
        IntervalTimer   m_LFGQueueUpdateTimer;              // update timer for statistic send

};

#define sLFGMgr MaNGOS::Singleton<LFGMgr>::Instance()
#endif
