/*
 * Copyright (C) 2005-2012 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2011-2013 MangosR2 <http://github.com/MangosR2>
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

#ifndef _ACCMGR_H
#define _ACCMGR_H

#include "Common.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"
#include "Policies/Singleton.h"
#include <mutex>
#include <string>

class Player;

enum AccountOpResult
{
    AOR_OK,
    AOR_NAME_TOO_LONG,
    AOR_PASS_TOO_LONG,
    AOR_NAME_ALREDY_EXIST,
    AOR_NAME_NOT_EXIST,
    AOR_DB_INTERNAL_ERROR
};

struct PlayerDataCache
{
    uint32      lowguid;
    uint32      account;
    std::string name;
    uint8       race;
};

#define MAX_ACCOUNT_STR 16

typedef UNORDERED_MAP<ObjectGuid, PlayerDataCache> PlayerDataCacheMap;
typedef std::vector<uint32> RafLinkedList;
typedef std::pair<uint32, bool> RafLinkedPair;
typedef UNORDERED_MAP<RafLinkedPair, RafLinkedList > RafLinkedMap;

HASH_NAMESPACE_START
template<> class hash <RafLinkedPair>
{
    public: size_t operator()(const RafLinkedPair& __x) const { return (size_t)(uint32(__x.second) << 31) | (__x.first); }
};
HASH_NAMESPACE_END

class AccountMgr : public MaNGOS::Singleton<AccountMgr, MaNGOS::ClassLevelLockable<AccountMgr, std::mutex> >
{
    public:
        AccountMgr();
        ~AccountMgr();

        AccountOpResult CreateAccount(std::string username, std::string password);
        AccountOpResult CreateAccount(std::string username, std::string password, uint32 expansion);
        AccountOpResult DeleteAccount(uint32 accid);
        AccountOpResult ChangeUsername(uint32 accid, std::string new_uname, std::string new_passwd);
        AccountOpResult ChangePassword(uint32 accid, std::string new_passwd);
        bool CheckPassword(uint32 accid, std::string passwd);

        uint32 GetId(std::string username);
        AccountTypes GetSecurity(uint32 acc_id);
        bool GetName(uint32 acc_id, std::string& name);
        uint32 GetCharactersCount(uint32 acc_id);
        std::string CalculateShaPassHash(std::string& name, std::string& password);

        ObjectGuid GetPlayerGuidByName(std::string name);
        bool GetPlayerNameByGUID(ObjectGuid guid, std::string &name);
        Team GetPlayerTeamByGUID(ObjectGuid guid);
        uint32 GetPlayerAccountIdByGUID(ObjectGuid guid);
        uint32 GetPlayerAccountIdByPlayerName(const std::string& name);

        uint32 GetCharactersCount(uint32 acc_id, bool full);
        void UpdateCharactersCount(uint32 acc_id, uint32 realm_id);

        void LockAccount(uint32 acc_id, bool lock);

        PlayerDataCache const* GetPlayerDataCache(ObjectGuid guid, bool force = true);
        PlayerDataCache const* GetPlayerDataCache(const std::string& name);
        void  ClearPlayerDataCache(ObjectGuid guid);
        void  MakePlayerDataCache(Player* player);

        RafLinkedList* GetRAFAccounts(uint32 accid, bool referred = true);
        AccountOpResult AddRAFLink(uint32 accid, uint32 friendid);
        AccountOpResult DeleteRAFLink(uint32 accid, uint32 friendid);

        static bool normalizeString(std::string& utf8str);

    private:
        PlayerDataCacheMap  mPlayerDataCacheMap;
        RafLinkedMap        mRAFLinkedMap;
};

#define sAccountMgr MaNGOS::Singleton<AccountMgr>::Instance()
#endif
