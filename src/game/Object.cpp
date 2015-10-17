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

#include "Object.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "Opcodes.h"
#include "Log.h"
#include "World.h"
#include "Creature.h"
#include "Player.h"
#include "Vehicle.h"
#include "ObjectMgr.h"
#include "ObjectGuid.h"
#include "UpdateData.h"
#include "UpdateMask.h"
#include "Util.h"
#include "MapManager.h"
#include "Log.h"
#include "Transports.h"
#include "TargetedMovementGenerator.h"
#include "WaypointMovementGenerator.h"
#include "VMapFactory.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "ObjectPosSelector.h"
#include "TemporarySummon.h"
#include "OutdoorPvP/OutdoorPvPMgr.h"
#include "movement/packet_builder.h"
#include "movement/MoveSplineInit.h"
#include "movement/MoveSpline.h"
#include "UpdateFieldFlags.h"
#include "Group.h"
#include "CreatureLinkingMgr.h"
#include "Chat.h"

#define TERRAIN_LOS_STEP_DISTANCE   3.0f        // sample distance for terrain LoS

UpdateFieldData::UpdateFieldData(Object const* object, Player* target)
{
    m_isSelf = object == target;
    m_isItemOwner = false;
    m_hasSpecialInfo = false;
    m_isPartyMember = false;

    switch (object->GetTypeId())
    {
        case TYPEID_ITEM:
        case TYPEID_CONTAINER:
            m_flags = ItemUpdateFieldFlags;
            m_isOwner = m_isItemOwner = ((Item*)object)->GetOwnerGuid() == target->GetObjectGuid();
            break;
        case TYPEID_UNIT:
        case TYPEID_PLAYER:
        {
            m_flags = UnitUpdateFieldFlags;
            m_isOwner = ((Unit*)object)->GetOwnerGuid() == target->GetObjectGuid();
            m_hasSpecialInfo = ((Unit*)object)->HasAuraTypeWithCaster(SPELL_AURA_EMPATHY, target->GetObjectGuid());
            if (Player* pPlayer = ((Unit*)object)->GetCharmerOrOwnerPlayerOrPlayerItself())
                m_isPartyMember = pPlayer->IsInSameRaidWith(target);
            break;
        }
        case TYPEID_GAMEOBJECT:
            m_flags = GameObjectUpdateFieldFlags;
            m_isOwner = ((GameObject*)object)->GetOwnerGuid() == target->GetObjectGuid();
            break;
        case TYPEID_DYNAMICOBJECT:
            m_flags = DynamicObjectUpdateFieldFlags;
            m_isOwner = ((DynamicObject*)object)->GetCasterGuid() == target->GetObjectGuid();
            break;
        case TYPEID_CORPSE:
            m_flags = CorpseUpdateFieldFlags;
            m_isOwner = ((Corpse*)object)->GetOwnerGuid() == target->GetObjectGuid();
            break;
    }
}

bool UpdateFieldData::IsUpdateFieldVisible(uint16 fieldIndex) const
{
    if (m_flags[fieldIndex] == UF_FLAG_NONE)
        return false;

    if (HasFlags(fieldIndex, UF_FLAG_PUBLIC) ||
        (HasFlags(fieldIndex, UF_FLAG_PRIVATE) && m_isSelf) ||
        (HasFlags(fieldIndex, UF_FLAG_OWNER) && m_isOwner) ||
        (HasFlags(fieldIndex, UF_FLAG_ITEM_OWNER) && m_isItemOwner) ||
        (HasFlags(fieldIndex, UF_FLAG_PARTY_MEMBER) && m_isPartyMember))
        return true;

    return false;
}

Object::Object() :
    m_objectType(TYPEMASK_OBJECT),
    m_objectTypeId(TYPEID_OBJECT),
    m_uint32Values(nullptr),
    m_valuesCount(0),
    m_fieldNotifyFlags(UF_FLAG_DYNAMIC),
    m_objectUpdated(false),
    m_skipUpdate(false),
    m_inWorld(false)
{
}

Object::~Object()
{
    if (IsInWorld())
    {
        ///- Do NOT call RemoveFromWorld here, if the object is a player it will crash
        sLog.outError("Object::~Object (%s type %u) deleted but still in world!!", GetObjectGuid() ? GetObjectGuid().GetString().c_str() : "<none>", GetTypeId());
        MANGOS_ASSERT(false);
    }

    if (m_objectUpdated)
    {
        sLog.outError("Object::~Object ((%s type %u) deleted but still have updated status!!", GetObjectGuid() ? GetObjectGuid().GetString().c_str() : "<none>", GetTypeId());
        MANGOS_ASSERT(false);
    }

    delete[] m_uint32Values;
}

void Object::_InitValues()
{
    m_uint32Values = new uint32[m_valuesCount];
    memset(m_uint32Values, 0, m_valuesCount * sizeof(uint32));

    m_changedValues.resize(m_valuesCount, false);

    m_objectUpdated = false;
}

void Object::_Create(ObjectGuid guid)
{
    if(!m_uint32Values)
        _InitValues();

    SetGuidValue(OBJECT_FIELD_GUID, guid);
    SetUInt32Value(OBJECT_FIELD_TYPE, m_objectType);
    m_PackGUID.Set(guid);
}

void Object::SetObjectScale(float newScale)
{
    SetFloatValue(OBJECT_FIELD_SCALE_X, newScale);
}

void Object::SendForcedObjectUpdate()
{
    if (!m_inWorld || !m_objectUpdated)
        return;

    UpdateDataMapType update_players;

    BuildUpdateData(update_players);
//    RemoveFromClientUpdateList();

    WorldPacket packet;                                     // here we allocate a std::vector with a size of 0x10000
    for (UpdateDataMapType::iterator iter = update_players.begin(); iter != update_players.end(); ++iter)
    {
        if (!iter->first || !iter->first.IsPlayer())
            continue;

        Player* pPlayer = ObjectAccessor::FindPlayer(iter->first);
        if (!pPlayer)
            continue;

        iter->second.BuildPacket(&packet);
        pPlayer->GetSession()->SendPacket(&packet);
    }
}

void Object::BuildMovementUpdateBlock(UpdateData * data, uint16 flags ) const
{
    ByteBuffer buf(500);

    buf << uint8(UPDATETYPE_MOVEMENT);
    buf << GetPackGUID();

    BuildMovementUpdate(&buf, flags);

    data->AddUpdateBlock(buf);
}

void Object::BuildCreateUpdateBlockForPlayer(UpdateData *data, Player *target) const
{
    if(!target)
        return;

    uint8  updatetype   = UPDATETYPE_CREATE_OBJECT;
    uint16 updateFlags  = m_updateFlag;

    /** lower flag1 **/
    if (target == this)                                      // building packet for yourself
        updateFlags |= UPDATEFLAG_SELF;

    if (updateFlags & UPDATEFLAG_HAS_POSITION)
    {
        // UPDATETYPE_CREATE_OBJECT2 dynamic objects, corpses...
        if (isType(TYPEMASK_DYNAMICOBJECT) || isType(TYPEMASK_CORPSE) || isType(TYPEMASK_PLAYER))
            updatetype = UPDATETYPE_CREATE_OBJECT2;

        // UPDATETYPE_CREATE_OBJECT2 for pets...
        if (target->GetPetGuid() == GetObjectGuid())
            updatetype = UPDATETYPE_CREATE_OBJECT2;

        // UPDATETYPE_CREATE_OBJECT2 for some gameobject types...
        if (isType(TYPEMASK_GAMEOBJECT))
        {
            switch(((GameObject*)this)->GetGoType())
            {
                case GAMEOBJECT_TYPE_TRAP:
                case GAMEOBJECT_TYPE_DUEL_ARBITER:
                case GAMEOBJECT_TYPE_FLAGSTAND:
                case GAMEOBJECT_TYPE_FLAGDROP:
                    updatetype = UPDATETYPE_CREATE_OBJECT2;
                    break;
                case GAMEOBJECT_TYPE_TRANSPORT:
                    updateFlags |= UPDATEFLAG_TRANSPORT;
                    break;
                default:
                    break;
            }
        }

        if (isType(TYPEMASK_UNIT))
        {
            if (((Unit*)this)->getVictim())
                updateFlags |= UPDATEFLAG_HAS_ATTACKING_TARGET;
        }
    }

    //DEBUG_LOG("BuildCreateUpdate: update-type: %u, object-type: %u got updateFlags: %X", updatetype, m_objectTypeId, updateFlags);

    ByteBuffer buf;
    buf << uint8(updatetype);
    buf << GetPackGUID();
    buf << uint8(m_objectTypeId);

    BuildMovementUpdate(&buf, updateFlags);

    UpdateMask updateMask;
    updateMask.SetCount(m_valuesCount);
    _SetCreateBits(&updateMask, target);
    BuildValuesUpdate(updatetype, &buf, &updateMask, target);
    data->AddUpdateBlock(buf);
}

void Object::SendCreateUpdateToPlayer(Player* player)
{
    // send create update to player
    UpdateData upd;
    WorldPacket packet;

    BuildCreateUpdateBlockForPlayer(&upd, player);
    upd.BuildPacket(&packet);
    player->GetSession()->SendPacket(&packet);
}

void Object::BuildValuesUpdateBlockForPlayer(UpdateData *data, Player *target) const
{
    ByteBuffer buf(500);

    buf << uint8(UPDATETYPE_VALUES);
    buf << GetPackGUID();

    UpdateMask updateMask;
    updateMask.SetCount(m_valuesCount);

    _SetUpdateBits(&updateMask, target);
    BuildValuesUpdate(UPDATETYPE_VALUES, &buf, &updateMask, target);

    data->AddUpdateBlock(buf);
}

void Object::BuildOutOfRangeUpdateBlock(UpdateData* data) const
{
    data->AddOutOfRangeGuid(GetObjectGuid());
}

void Object::DestroyForPlayer( Player *target, bool anim ) const
{
    MANGOS_ASSERT(target);

    WorldPacket data(SMSG_DESTROY_OBJECT, 9);
    data << GetObjectGuid();
    data << uint8(anim ? 1 : 0);                            // WotLK (bool), may be despawn animation
    target->GetSession()->SendPacket(&data);
}

void Object::BuildMovementUpdate(ByteBuffer * data, uint16 updateFlags) const
{

    *data << uint16(updateFlags);                           // update flags

    // 0x20
    if (updateFlags & UPDATEFLAG_LIVING)
    {
        Unit* unit = ((Unit*)this);

        if (unit->IsOnTransport() || unit->GetVehicle())
            unit->m_movementInfo.AddMovementFlag(MOVEFLAG_ONTRANSPORT);
        else
            unit->m_movementInfo.RemoveMovementFlag(MOVEFLAG_ONTRANSPORT);

        // Update movement info time
        unit->m_movementInfo.UpdateTime(WorldTimer::getMSTime());
        // Write movement info
        unit->m_movementInfo.Write(*data);

        // Unit speeds
        *data << float(unit->GetSpeed(MOVE_WALK));
        *data << float(unit->GetSpeed(MOVE_RUN));
        *data << float(unit->GetSpeed(MOVE_RUN_BACK));
        *data << float(unit->GetSpeed(MOVE_SWIM));
        *data << float(unit->GetSpeed(MOVE_SWIM_BACK));
        *data << float(unit->GetSpeed(MOVE_FLIGHT));
        *data << float(unit->GetSpeed(MOVE_FLIGHT_BACK));
        *data << float(unit->GetSpeed(MOVE_TURN_RATE));
        *data << float(unit->GetSpeed(MOVE_PITCH_RATE));

        // 0x08000000
        if (unit->m_movementInfo.GetMovementFlags() & MOVEFLAG_SPLINE_ENABLED)
            Movement::PacketBuilder::WriteCreate(*unit->movespline, *data);
    }
    else
    {
        if (updateFlags & UPDATEFLAG_POSITION)
        {
            ObjectGuid transportGuid;
            if (((WorldObject*)this)->GetTransportInfo())
                transportGuid = ((WorldObject*)this)->GetTransportInfo()->GetTransportGuid();

            if (transportGuid)
                *data << transportGuid.WriteAsPacked();
            else
                *data << uint8(0);

            *data << float(((WorldObject*)this)->GetPositionX());
            *data << float(((WorldObject*)this)->GetPositionY());
            *data << float(((WorldObject*)this)->GetPositionZ());

            if (transportGuid)
            {
                *data << float(((WorldObject*)this)->GetTransOffsetX());
                *data << float(((WorldObject*)this)->GetTransOffsetY());
                *data << float(((WorldObject*)this)->GetTransOffsetZ());
            }
            else
            {
                *data << float(((WorldObject*)this)->GetPositionX());
                *data << float(((WorldObject*)this)->GetPositionY());
                *data << float(((WorldObject*)this)->GetPositionZ());
            }
            *data << float(((WorldObject*)this)->GetOrientation());

            if (GetTypeId() == TYPEID_CORPSE)
                *data << float(((WorldObject*)this)->GetOrientation());
            else
                *data << float(0);
        }
        else
        {
            // 0x40
            if (updateFlags & UPDATEFLAG_HAS_POSITION)
            {
                // 0x02
                if (updateFlags & UPDATEFLAG_TRANSPORT && !GetObjectGuid().IsMOTransport())
                {
                    *data << float(((WorldObject*)this)->GetTransOffsetX());
                    *data << float(((WorldObject*)this)->GetTransOffsetY());
                    *data << float(((WorldObject*)this)->GetTransOffsetZ());
                    *data << float(((WorldObject*)this)->GetTransOffsetO());
                }
                else
                {
                    *data << float(((WorldObject*)this)->GetPositionX());
                    *data << float(((WorldObject*)this)->GetPositionY());
                    *data << float(((WorldObject*)this)->GetPositionZ());
                    *data << float(((WorldObject*)this)->GetOrientation());
                }
            }
        }
    }

    // 0x8
    if (updateFlags & UPDATEFLAG_LOWGUID)
    {
        switch(GetTypeId())
        {
            case TYPEID_OBJECT:
            case TYPEID_ITEM:
            case TYPEID_CONTAINER:
            case TYPEID_GAMEOBJECT:
            case TYPEID_DYNAMICOBJECT:
            case TYPEID_CORPSE:
                *data << uint32(GetGUIDLow());              // GetGUIDLow()
                break;
            case TYPEID_UNIT:
                *data << uint32(0x0000000B);                // unk, can be 0xB or 0xC
                break;
            case TYPEID_PLAYER:
                if (updateFlags & UPDATEFLAG_SELF)
                    *data << uint32(0x0000002F);            // unk, can be 0x15 or 0x22
                else
                    *data << uint32(0x00000008);            // unk, can be 0x7 or 0x8
                break;
            default:
                *data << uint32(0x00000000);                // unk
                break;
        }
    }

    // 0x10
    if (updateFlags & UPDATEFLAG_HIGHGUID)
    {
        switch(GetTypeId())
        {
            case TYPEID_OBJECT:
            case TYPEID_ITEM:
            case TYPEID_CONTAINER:
            case TYPEID_GAMEOBJECT:
            case TYPEID_DYNAMICOBJECT:
            case TYPEID_CORPSE:
                *data << uint32(GetObjectGuid().GetHigh()); // GetGUIDHigh()
                break;
            case TYPEID_UNIT:
                *data << uint32(0x0000000B);                // unk, can be 0xB or 0xC
                break;
            case TYPEID_PLAYER:
                if (updateFlags & UPDATEFLAG_SELF)
                    *data << uint32(0x0000002F);            // unk, can be 0x15 or 0x22
                else
                    *data << uint32(0x00000008);            // unk, can be 0x7 or 0x8
                break;
            default:
                *data << uint32(0x00000000);                // unk
                break;
        }
    }

    // 0x4
    if (updateFlags & UPDATEFLAG_HAS_ATTACKING_TARGET)       // packed guid (current target guid)
    {
        if (Unit* pVictim = ((Unit*)this)->getVictim())
            *data << pVictim->GetPackGUID();
        else
            data->appendPackGUID(0);
    }

    // 0x2
    if (updateFlags & UPDATEFLAG_TRANSPORT)
    {
        *data << uint32(WorldTimer::getMSTime());                       // ms time
    }

    // 0x80
    if (updateFlags & UPDATEFLAG_VEHICLE)
    {
        *data << uint32(((Unit*)this)->GetVehicleInfo()->m_ID); // vehicle id
        if (((WorldObject*)this)->IsOnTransport())
            *data << float(((WorldObject*)this)->GetTransOffsetO());
        else
            *data << float(((WorldObject*)this)->GetOrientation());
    }

    // 0x200
    if (updateFlags & UPDATEFLAG_ROTATION)
    {
        *data << int64(((GameObject*)this)->GetPackedWorldRotation());
    }
}

void Object::BuildValuesUpdate(uint8 updatetype, ByteBuffer * data, UpdateMask *updateMask, Player *target) const
{
    if (!target)
        return;

    bool IsActivateToQuest = false;
    bool IsPerCasterAuraState = false;

    if (updatetype == UPDATETYPE_CREATE_OBJECT || updatetype == UPDATETYPE_CREATE_OBJECT2)
    {
        if (isType(TYPEMASK_GAMEOBJECT) && !((GameObject*)this)->IsDynTransport())
        {
            if (((GameObject*)this)->ActivateToQuest(target) || target->isGameMaster())
                IsActivateToQuest = true;
        }
        else if (isType(TYPEMASK_UNIT))
        {
            if (((Unit*)this)->HasAuraState(AURA_STATE_CONFLAGRATE))
            {
                IsPerCasterAuraState = true;
                updateMask->SetBit(UNIT_FIELD_AURASTATE);
            }
        }
    }
    else                                                    // case UPDATETYPE_VALUES
    {
        if (isType(TYPEMASK_GAMEOBJECT) && !((GameObject*)this)->IsDynTransport())
        {
            if (((GameObject*)this)->ActivateToQuest(target) || target->isGameMaster())
                IsActivateToQuest = true;
            updateMask->SetBit(GAMEOBJECT_BYTES_1);         // why do we need this here?
        }
        else if (isType(TYPEMASK_UNIT))
        {
            if (((Unit*)this)->HasAuraState(AURA_STATE_CONFLAGRATE))
            {
                IsPerCasterAuraState = true;
                updateMask->SetBit(UNIT_FIELD_AURASTATE);
            }
        }
    }

    MANGOS_ASSERT(updateMask && updateMask->GetCount() == m_valuesCount);

    *data << (uint8)updateMask->GetBlockCount();
    data->append(updateMask->GetMask(), updateMask->GetLength());

    // 2 specialized loops for speed optimization in non-unit case
    if (isType(TYPEMASK_UNIT))                              // unit (creature/player) case
    {
        for(uint16 index = 0; index < m_valuesCount; ++index)
        {
            if (updateMask->GetBit(index))
            {
                if (index == UNIT_NPC_FLAGS)
                {
                    uint32 appendValue = m_uint32Values[index];

                    if (GetTypeId() == TYPEID_UNIT)
                    {
                        if (!target->canSeeSpellClickOn((Creature*)this))
                            appendValue &= ~UNIT_NPC_FLAG_SPELLCLICK;

                        if (appendValue & UNIT_NPC_FLAG_TRAINER)
                        {
                            if (!((Creature*)this)->IsTrainerOf(target, false))
                                appendValue &= ~(UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_TRAINER_CLASS | UNIT_NPC_FLAG_TRAINER_PROFESSION);
                        }

                        if (appendValue & UNIT_NPC_FLAG_STABLEMASTER)
                        {
                            if (target->getClass() != CLASS_HUNTER)
                                appendValue &= ~UNIT_NPC_FLAG_STABLEMASTER;
                        }
                    }

                    *data << uint32(appendValue);
                }
                else if (index == UNIT_FIELD_AURASTATE)
                {
                    if (IsPerCasterAuraState)
                    {
                        // IsPerCasterAuraState set if related pet caster aura state set already
                        if (((Unit*)this)->HasAuraStateForCaster(AURA_STATE_CONFLAGRATE, target->GetObjectGuid()))
                            *data << m_uint32Values[index];
                        else
                            *data << (m_uint32Values[index] & ~(1 << (AURA_STATE_CONFLAGRATE-1)));
                    }
                    else
                        *data << m_uint32Values[index];
                }
                // FIXME: Some values at server stored in float format but must be sent to client in uint32 format
                else if (index >= UNIT_FIELD_BASEATTACKTIME && index <= UNIT_FIELD_RANGEDATTACKTIME)
                {
                    // convert from float to uint32 and send
                    *data << uint32(m_floatValues[index] < 0 ? 0 : m_floatValues[index]);
                }

                // there are some float values which may be negative or can't get negative due to other checks
                else if ((index >= UNIT_FIELD_NEGSTAT0 && index <= UNIT_FIELD_NEGSTAT4) ||
                    (index >= UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE  && index <= (UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE + 6)) ||
                    (index >= UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE  && index <= (UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE + 6)) ||
                    (index >= UNIT_FIELD_POSSTAT0 && index <= UNIT_FIELD_POSSTAT4))
                {
                    *data << uint32(m_floatValues[index]);
                }

                // Gamemasters should be always able to select units - remove not selectable flag
                else if (index == UNIT_FIELD_FLAGS && target->isGameMaster())
                {
                    *data << (m_uint32Values[index] & ~UNIT_FLAG_NOT_SELECTABLE);
                }
                // Hide special-info for non empathy-casters,
                // Hide lootable animation for unallowed players
                else if (index == UNIT_DYNAMIC_FLAGS)
                {
                    uint32 dynflagsValue = m_uint32Values[index];

                    // Checking SPELL_AURA_EMPATHY and caster
                    if (dynflagsValue & UNIT_DYNFLAG_SPECIALINFO && ((Unit*)this)->isAlive())
                    {
                        bool bIsEmpathy = false;
                        bool bIsCaster = false;
                        Unit::AuraList const& mAuraEmpathy = ((Unit*)this)->GetAurasByType(SPELL_AURA_EMPATHY);
                        for (Unit::AuraList::const_iterator itr = mAuraEmpathy.begin(); !bIsCaster && itr != mAuraEmpathy.end(); ++itr)
                        {
                            bIsEmpathy = true;              // Empathy by aura set
                            if ((*itr)->GetCasterGuid() == target->GetObjectGuid())
                                bIsCaster = true;           // target is the caster of an empathy aura
                        }
                        if (bIsEmpathy && !bIsCaster)       // Empathy by aura, but target is not the caster
                            dynflagsValue &= ~UNIT_DYNFLAG_SPECIALINFO;
                    }

                    // Checking lootable
                    if (dynflagsValue & UNIT_DYNFLAG_LOOTABLE && GetTypeId() == TYPEID_UNIT)
                    {
                        if (!target->isAllowedToLoot((Creature*)this))
                            dynflagsValue &= ~(UNIT_DYNFLAG_LOOTABLE | UNIT_DYNFLAG_TAPPED_BY_PLAYER);
                        else
                        {
                            // flag only for original loot recipent
                            if (target->GetObjectGuid() != ((Creature*)this)->GetLootRecipientGuid())
                                dynflagsValue &= ~(UNIT_DYNFLAG_TAPPED | UNIT_DYNFLAG_TAPPED_BY_PLAYER);
                        }
                    }

                    *data << dynflagsValue;
                }
                else                                        // Unhandled index, just send
                {
                    // send in current format (float as float, uint32 as uint32)
                    *data << m_uint32Values[index];
                }
            }
        }
    }
    else if (isType(TYPEMASK_GAMEOBJECT))                   // gameobject case
    {
        for(uint16 index = 0; index < m_valuesCount; ++index)
        {
            if (updateMask->GetBit(index))
            {
                // send in current format (float as float, uint32 as uint32)
                if (index == GAMEOBJECT_DYNAMIC)
                {
                    // GAMEOBJECT_TYPE_DUNGEON_DIFFICULTY can have lo flag = 2
                    //      most likely related to "can enter map" and then should be 0 if can not enter

                    switch(((GameObject*)this)->GetGoType())
                    {
                        case GAMEOBJECT_TYPE_QUESTGIVER:
                            // GO also seen with GO_DYNFLAG_LO_SPARKLE explicit, relation/reason unclear (192861)
                            *data << uint16(IsActivateToQuest ? GO_DYNFLAG_LO_ACTIVATE : GO_DYNFLAG_LO_NONE);
                            *data << uint16(-1);
                            break;
                        case GAMEOBJECT_TYPE_CHEST:
                        case GAMEOBJECT_TYPE_GENERIC:
                        case GAMEOBJECT_TYPE_SPELL_FOCUS:
                        case GAMEOBJECT_TYPE_GOOBER:
                            *data << uint16(IsActivateToQuest ? (GO_DYNFLAG_LO_ACTIVATE | GO_DYNFLAG_LO_SPARKLE) : GO_DYNFLAG_LO_NONE);
                            *data << uint16(-1);
                            break;
                        case GAMEOBJECT_TYPE_TRANSPORT:
                        case GAMEOBJECT_TYPE_MO_TRANSPORT:
                            *data << uint16(((GameObject*)this)->GetGoState() != GO_STATE_ACTIVE ? GO_DYNFLAG_LO_TRANSPORT_STOP : GO_DYNFLAG_LO_NONE);
                            *data << uint16(-1);
                            break;
                        default:
                            // unknown, not happen.
                            *data << uint16(GO_DYNFLAG_LO_NONE);
                            *data << uint16(-1);
                            break;
                    }
                }
                else
                    *data << m_uint32Values[index];         // other cases
            }
        }
    }
    else                                                    // other objects case (no special index checks)
    {
        for(uint16 index = 0; index < m_valuesCount; ++index)
        {
            if (updateMask->GetBit(index))
            {
                // send in current format (float as float, uint32 as uint32)
                *data << m_uint32Values[index];
            }
        }
    }
}

void Object::ClearUpdateMask(bool remove)
{
    if (m_uint32Values)
    {
        for (uint16 index = 0; index < m_valuesCount; ++index)
            m_changedValues[index] = false;
    }

    if (m_objectUpdated)
    {
        if (remove)
            RemoveFromClientUpdateList();
        m_objectUpdated = false;
    }
}

bool Object::LoadValues(const char* data)
{
    if (!m_uint32Values)
        _InitValues();

    Tokens tokens(data, ' ');

    if (tokens.size() != m_valuesCount)
        return false;

    for (uint16 index = 0; index < m_valuesCount; ++index)
    {
        m_uint32Values[index] = atol(tokens[index]);
    }

    return true;
}

void Object::_SetUpdateBits(UpdateMask* updateMask, Player* target) const
{
    UpdateFieldData ufd(this, target);

    for (uint16 index = 0; index < m_valuesCount; ++index)
    {
        if (ufd.IsUpdateNeeded(index, m_fieldNotifyFlags) ||
            (m_changedValues[index] && ufd.IsUpdateFieldVisible(index)))
            updateMask->SetBit(index);
    }
}

void Object::_SetCreateBits(UpdateMask* updateMask, Player* target) const
{
    UpdateFieldData ufd(this, target);

    for (uint16 index = 0; index < m_valuesCount; ++index)
    {
        if (ufd.IsUpdateNeeded(index, m_fieldNotifyFlags) ||
            ((GetUInt32Value(index) != 0) && ufd.IsUpdateFieldVisible(index)))
            updateMask->SetBit(index);
    }
}

void Object::SetInt32Value( uint16 index, int32 value )
{
    MANGOS_ASSERT( index < m_valuesCount || PrintIndexError( index, true ) );

    if (m_int32Values[index] != value)
    {
        m_int32Values[index] = value;
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::SetUInt32Value( uint16 index, uint32 value )
{
    MANGOS_ASSERT( index < m_valuesCount || PrintIndexError( index, true ) );

    if (m_uint32Values[index] != value)
    {
        m_uint32Values[index] = value;
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::SetUInt64Value( uint16 index, const uint64 &value )
{
    MANGOS_ASSERT(index + 1 < m_valuesCount || PrintIndexError(index, true));
    if (*((uint64*) & (m_uint32Values[index])) != value)
    {
        m_uint32Values[index] = *((uint32*)&value);
        m_uint32Values[index + 1] = *(((uint32*)&value) + 1);
        m_changedValues[index] = true;
        m_changedValues[index + 1] = true;
        MarkForClientUpdate();
    }
}

void Object::SetFloatValue( uint16 index, float value )
{
    MANGOS_ASSERT( index < m_valuesCount || PrintIndexError( index, true ) );

    if (m_floatValues[index] != value)
    {
        m_floatValues[index] = value;
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::SetByteValue( uint16 index, uint8 offset, uint8 value )
{
    MANGOS_ASSERT( index < m_valuesCount || PrintIndexError( index, true ) );

    if (offset > 4)
    {
        sLog.outError("Object::SetByteValue: wrong offset %u", offset);
        return;
    }

    if (uint8(m_uint32Values[index] >> (offset * 8)) != value)
    {
        m_uint32Values[index] &= ~uint32(uint32(0xFF) << (offset * 8));
        m_uint32Values[index] |= uint32(uint32(value) << (offset * 8));
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::SetUInt16Value( uint16 index, uint8 offset, uint16 value )
{
    MANGOS_ASSERT( index < m_valuesCount || PrintIndexError( index, true ) );

    if (offset > 2)
    {
        sLog.outError("Object::SetUInt16Value: wrong offset %u", offset);
        return;
    }

    if (uint16(m_uint32Values[index] >> (offset * 16)) != value)
    {
        m_uint32Values[index] &= ~uint32(uint32(0xFFFF) << (offset * 16));
        m_uint32Values[index] |= uint32(uint32(value) << (offset * 16));
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::SetStatFloatValue( uint16 index, float value)
{
    if (value < 0)
        value = 0.0f;

    SetFloatValue(index, value);
}

void Object::SetStatInt32Value( uint16 index, int32 value)
{
    if (value < 0)
        value = 0;

    SetUInt32Value(index, uint32(value));
}

void Object::ApplyModUInt32Value(uint16 index, int32 val, bool apply)
{
    int32 cur = GetUInt32Value(index);
    cur += (apply ? val : -val);
    if (cur < 0)
        cur = 0;
    SetUInt32Value(index, cur);
}

void Object::ApplyModInt32Value(uint16 index, int32 val, bool apply)
{
    int32 cur = GetInt32Value(index);
    cur += (apply ? val : -val);
    SetInt32Value(index, cur);
}

void Object::ApplyModSignedFloatValue(uint16 index, float  val, bool apply)
{
    float cur = GetFloatValue(index);
    cur += (apply ? val : -val);
    SetFloatValue(index, cur);
}

void Object::ApplyModPositiveFloatValue(uint16 index, float  val, bool apply)
{
    float cur = GetFloatValue(index);
    cur += (apply ? val : -val);
    if (cur < 0)
        cur = 0;
    SetFloatValue(index, cur);
}

void Object::SetFlag( uint16 index, uint32 newFlag )
{
    MANGOS_ASSERT(index < m_valuesCount || PrintIndexError(index, true));
    uint32 oldval = m_uint32Values[index];
    uint32 newval = oldval | newFlag;

    if (oldval != newval)
    {
        m_uint32Values[index] = newval;
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::RemoveFlag( uint16 index, uint32 oldFlag )
{
    MANGOS_ASSERT(index < m_valuesCount || PrintIndexError(index, true));
    uint32 oldval = m_uint32Values[index];
    uint32 newval = oldval & ~oldFlag;

    if (oldval != newval)
    {
        m_uint32Values[index] = newval;
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::SetByteFlag( uint16 index, uint8 offset, uint8 newFlag )
{
    MANGOS_ASSERT( index < m_valuesCount || PrintIndexError( index, true ) );

    if (offset > 4)
    {
        sLog.outError("Object::SetByteFlag: wrong offset %u", offset);
        return;
    }

    if (!(uint8(m_uint32Values[index] >> (offset * 8)) & newFlag))
    {
        m_uint32Values[index] |= uint32(uint32(newFlag) << (offset * 8));
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::RemoveByteFlag( uint16 index, uint8 offset, uint8 oldFlag )
{
    MANGOS_ASSERT( index < m_valuesCount || PrintIndexError( index, true ) );

    if (offset > 4)
    {
        sLog.outError("Object::RemoveByteFlag: wrong offset %u", offset);
        return;
    }

    if (uint8(m_uint32Values[index] >> (offset * 8)) & oldFlag)
    {
        m_uint32Values[index] &= ~uint32(uint32(oldFlag) << (offset * 8));
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::SetShortFlag(uint16 index, bool highpart, uint16 newFlag)
{
    MANGOS_ASSERT(index < m_valuesCount || PrintIndexError(index, true));

    if (!(uint16(m_uint32Values[index] >> (highpart ? 16 : 0)) & newFlag))
    {
        m_uint32Values[index] |= uint32(uint32(newFlag) << (highpart ? 16 : 0));
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

void Object::RemoveShortFlag(uint16 index, bool highpart, uint16 oldFlag)
{
    MANGOS_ASSERT(index < m_valuesCount || PrintIndexError(index, true));

    if (uint16(m_uint32Values[index] >> (highpart ? 16 : 0)) & oldFlag)
    {
        m_uint32Values[index] &= ~uint32(uint32(oldFlag) << (highpart ? 16 : 0));
        m_changedValues[index] = true;
        MarkForClientUpdate();
    }
}

bool Object::PrintIndexError(uint32 index, bool set) const
{
    sLog.outError("Attempt %s nonexistent value field: %u (count: %u) for object typeid: %u type mask: %u",(set ? "set value to" : "get value from"),index,m_valuesCount,GetTypeId(),m_objectType);

    // ASSERT must fail after function call
    return false;
}

bool Object::PrintEntryError(char const* descr) const
{
    sLog.outError("Object Type %u, Entry %u (lowguid %u) with invalid call for %s", GetTypeId(), GetEntry(), GetObjectGuid().GetCounter(), descr);

    // always false for continue assert fail
    return false;
}


void Object::BuildUpdateDataForPlayer(Player* player, UpdateDataMapType& update_players)
{
    if (!player)
        return;

    UpdateData& data = update_players[player->GetObjectGuid()];

    BuildValuesUpdateBlockForPlayer(&data, player);
}

void Object::AddToClientUpdateList()
{
    sLog.outError("Unexpected call of Object::AddToClientUpdateList for object (TypeId: %u Update fields: %u)",GetTypeId(), m_valuesCount);
    MANGOS_ASSERT(false);
}

void Object::RemoveFromClientUpdateList()
{
    sLog.outError("Unexpected call of Object::RemoveFromClientUpdateList for object (TypeId: %u Update fields: %u)",GetTypeId(), m_valuesCount);
    MANGOS_ASSERT(false);
}

void Object::BuildUpdateData( UpdateDataMapType& /*update_players */)
{
    sLog.outError("Unexpected call of Object::BuildUpdateData for object (TypeId: %u Update fields: %u)",GetTypeId(), m_valuesCount);
    MANGOS_ASSERT(false);
}

void Object::MarkForClientUpdate()
{
    if (m_inWorld)
    {
        if(!m_objectUpdated)
        {
            AddToClientUpdateList();
            m_objectUpdated = true;
        }
    }
}

WorldObject::WorldObject() :
  movespline(new Movement::MoveSpline()), m_transportInfo(nullptr), 
  m_currMap(nullptr), m_position(WorldLocation()), m_viewPoint(*this),
  m_isActiveObject(false), m_LastUpdateTime(WorldTimer::getMSTime()){}

WorldObject::~WorldObject()
{
    delete movespline;
}

void WorldObject::CleanupsBeforeDelete()
{
    RemoveFromWorld(false);
    ClearUpdateMask(true);
}

void WorldObject::_Create(ObjectGuid guid, uint32 phaseMask)
{
    Object::_Create(guid);
    SetPhaseMask(phaseMask, false);
}

void WorldObject::AddToWorld()
{
    MANGOS_ASSERT(m_currMap);
    if (!IsInWorld())
        Object::AddToWorld();

    // Possible inserted object, already exists in object store. Not must cause any problem, but need check.
    GetMap()->InsertObject(this);
    GetMap()->AddUpdateObject(GetObjectGuid());
}

void WorldObject::RemoveFromWorld(bool remove)
{
    Map* map = GetMap();
    MANGOS_ASSERT(map);

    if (IsInWorld())
        Object::RemoveFromWorld(remove);

    map->RemoveUpdateObject(GetObjectGuid());

    if (remove)
    {
        ResetMap();
        map->EraseObject(GetObjectGuid());
    }
}

// Attention! This method cannot must call while relocation to other map!
void WorldObject::Relocate(Position const& position)
{
    bool positionChanged    = !bool(position == m_position);
    bool orientationChanged = bool(fabs(position.o - m_position.o) > M_NULL_F);

    m_position = position;

    if (isType(TYPEMASK_UNIT))
    {
        if (positionChanged)
            ((Unit*)this)->m_movementInfo.ChangePosition(m_position);
        else if (orientationChanged)
            ((Unit*)this)->m_movementInfo.ChangeOrientation(m_position.o);
    }
}

void WorldObject::SetOrientation(float orientation)
{
    Relocate(Position(GetPositionX(), GetPositionY(), GetPositionZ(), orientation, GetPhaseMask()));
}

void WorldObject::Relocate(WorldLocation const& location)
{
    if (location.HasMap() && GetMapId() != location.GetMapId())
    {
        if (sMapMgr.IsValidMAP(location.GetMapId()))
        {
            SetLocationMapId(location.GetMapId());
            if (GetInstanceId() != location.GetInstanceId())
            {
                if (!sMapMgr.FindMap(location.GetMapId(),location.GetInstanceId()))
                    DEBUG_LOG("WorldObject::Relocate %s try relocate to non-existance instance %u (map %u).", GetObjectGuid().GetString().c_str(),location.GetInstanceId(), location.GetMapId());
                SetLocationInstanceId(location.GetInstanceId());
            }
        }
        else
            sLog.outError("WorldObject::Relocate %s try relocate to non-existance map %u!", GetObjectGuid().GetString().c_str(), location.GetMapId());
    }

    Relocate(Position(location));
}

uint32 WorldObject::GetZoneId() const
{
    return GetTerrain()->GetZoneId(m_position.x, m_position.y, m_position.z);
}

uint32 WorldObject::GetAreaId() const
{
    return GetTerrain()->GetAreaId(m_position.x, m_position.y, m_position.z);
}

void WorldObject::GetZoneAndAreaId(uint32& zoneid, uint32& areaid) const
{
    GetTerrain()->GetZoneAndAreaId(zoneid, areaid, m_position.x, m_position.y, m_position.z);
}

InstanceData* WorldObject::GetInstanceData() const
{
    return GetMap()->GetInstanceData();
}

float WorldObject::GetDistance(WorldObject const* obj) const
{
    if (!obj)
        return (MAX_VISIBILITY_DISTANCE + 1.0f);

    float sizefactor = obj->GetObjectBoundingRadius();
    float dist = GetDistance(obj->GetPosition()) - sizefactor;
    return (dist > M_NULL_F ? dist : 0.0f);
}

float WorldObject::GetDistance(WorldLocation const& loc) const
{
    float sizefactor = GetObjectBoundingRadius();
    float dist = GetPosition().GetDistance(loc) - sizefactor;
    return (dist > M_NULL_F ? dist : 0.0f);
}

float WorldObject::GetDistance2d(float x, float y) const
{
    float sizefactor = GetObjectBoundingRadius();
    float dist = GetPosition().GetDistance(Location(x, y, GetPositionZ())) - sizefactor;
    return (dist > M_NULL_F ? dist : 0.0f);
}

float WorldObject::GetDistance(float x, float y, float z) const
{
    float sizefactor = GetObjectBoundingRadius();
    float dist = GetPosition().GetDistance(Location(x, y, z)) - sizefactor;
    return (dist > M_NULL_F ? dist : 0.0f);
}

float WorldObject::GetDistance2d(WorldObject const* obj) const
{
    if (!obj)
        return (MAX_VISIBILITY_DISTANCE + 1.0f);

    float sizefactor = GetObjectBoundingRadius() + obj->GetObjectBoundingRadius();
    float dist = GetPosition().GetDistance(Location(obj->GetPositionX(), obj->GetPositionY(), GetPositionZ())) - sizefactor;
    return (dist > M_NULL_F ? dist : 0.0f);
}

float WorldObject::GetDistanceZ(WorldObject const* obj) const
{
    if (!obj)
        return (MAX_VISIBILITY_DISTANCE + 1.0f);

    float dz = fabs(GetPositionZ() - obj->GetPositionZ());
    float sizefactor = GetObjectBoundingRadius() + obj->GetObjectBoundingRadius();
    float dist = dz - sizefactor;
    return (dist > M_NULL_F ? dist : 0.0f);
}

bool WorldObject::IsWithinDist3d(float x, float y, float z, float dist2compare) const
{
    return IsWithinDist3d(Location(x, y, z), dist2compare);
}

bool WorldObject::IsWithinDist3d(Location const& loc, float dist2compare) const
{
    float sizefactor = GetObjectBoundingRadius();
    float dist = GetPosition().GetDistance(loc);
    float maxdist = dist2compare + sizefactor;

    return dist < maxdist;
}

bool WorldObject::IsWithinDist2d(float x, float y, float dist2compare) const
{
    float dist = GetPosition().GetDistance(Location(x, y, GetPositionZ()));
    float sizefactor = GetObjectBoundingRadius();
    float maxdist = dist2compare + sizefactor;

    return dist < maxdist;
}

bool WorldObject::_IsWithinDist(WorldObject const* obj, float dist2compare, bool is3D) const
{

    float dist = is3D ?
        GetPosition().GetDistance(obj->GetPosition()) :
        GetPosition().GetDistance(Location(obj->GetPositionX(), obj->GetPositionY(), GetPositionZ()));

    float sizefactor = GetObjectBoundingRadius() + obj->GetObjectBoundingRadius();
    float maxdist = dist2compare + sizefactor;

    return dist < maxdist;
}

bool WorldObject::IsWithinLOSInMap(WorldObject const* obj) const
{
    if (!IsInMap(obj))
        return false;

    float ox,oy,oz;
    obj->GetPosition(ox,oy,oz);
    return IsWithinLOS(ox, oy, oz);
}

bool WorldObject::IsWithinLOS(float ox, float oy, float oz) const
{
    float x,y,z;
    GetPosition(x,y,z);
    return GetMap()->IsInLineOfSight(x, y, z + 2.0f, ox, oy, oz + 2.0f, GetPhaseMask());
}

bool WorldObject::GetDistanceOrder(WorldObject const* obj1, WorldObject const* obj2, bool is3D /* = true */) const
{
    float dist1 = is3D ?
        GetPosition().GetDistance(obj1->GetPosition()) :
        GetPosition().GetDistance(Location(obj1->GetPositionX(), obj1->GetPositionY(), GetPositionZ()));

    float dist2 = is3D ?
        GetPosition().GetDistance(obj2->GetPosition()) :
        GetPosition().GetDistance(Location(obj2->GetPositionX(), obj2->GetPositionY(), GetPositionZ()));

    return dist1 < dist2;
}

bool WorldObject::IsInRange(WorldObject const* obj, float minRange, float maxRange, bool is3D /* = true */) const
{
    float dist = is3D ?
        GetPosition().GetDistance(obj->GetPosition()) :
        GetPosition().GetDistance(Location(obj->GetPositionX(), obj->GetPositionY(), GetPositionZ()));

    float sizefactor = GetObjectBoundingRadius() + obj->GetObjectBoundingRadius();

    // check only for real range
    if (minRange > M_NULL_F)
    {
        float mindist = minRange + sizefactor;
        if (dist < mindist)
            return false;
    }

    float maxdist = maxRange + sizefactor;
    return dist < maxdist;
}

bool WorldObject::IsInRange2d(float x, float y, float minRange, float maxRange) const
{
    float dist = GetPosition().GetDistance(Location(x, y, GetPositionZ()));

    float sizefactor = GetObjectBoundingRadius();

    // check only for real range
    if (minRange > M_NULL_F)
    {
        float mindist = minRange + sizefactor;
        if (dist < mindist)
            return false;
    }

    float maxdist = maxRange + sizefactor;
    return dist < maxdist;
}

bool WorldObject::IsInRange3d(float x, float y, float z, float minRange, float maxRange) const
{

    float dist = GetPosition().GetDistance(Location(x, y, z));
    float sizefactor = GetObjectBoundingRadius();

    // check only for real range
    if (minRange > M_NULL_F)
    {
        float mindist = minRange + sizefactor;
        if (dist < mindist)
            return false;
    }

    float maxdist = maxRange + sizefactor;
    return dist < maxdist;
}

bool WorldObject::IsInBetween(WorldObject const* obj1, WorldObject const* obj2, float size) const
{
    if (GetPositionX() > std::max(obj1->GetPositionX(), obj2->GetPositionX())
        || GetPositionX() < std::min(obj1->GetPositionX(), obj2->GetPositionX())
        || GetPositionY() > std::max(obj1->GetPositionY(), obj2->GetPositionY())
        || GetPositionY() < std::min(obj1->GetPositionY(), obj2->GetPositionY()))
        return false;

    if (!size)
        size = GetObjectBoundingRadius() / 2;

    float angle = obj1->GetAngle(this) - obj1->GetAngle(obj2);
    return fabs(sin(angle)) * GetExactDist2d(obj1->GetPositionX(), obj1->GetPositionY()) < size;
}

float WorldObject::GetAngle(WorldObject const* obj) const
{
    if (!obj)
        return 0.0f;

    // Rework the assert, when more cases where such a call can happen have been fixed
    // MANGOS_ASSERT(obj != this || PrintEntryError("GetAngle (for self)"));
    if (obj == this)
    {
        sLog.outError("WorldObject::GetAngle INVALID CALL for GetAngle for %s", obj->GetGuidStr().c_str());
        return 0.0f;
    }
    return GetAngle(obj->GetPositionX(), obj->GetPositionY());
}

// Return angle in range 0..2*pi
float WorldObject::GetAngle(float const x, float const y) const
{
    float dx = x - GetPositionX();
    float dy = y - GetPositionY();

    float ang = atan2(dy, dx);                              // returns value between -Pi..Pi
    ang = (ang >= 0.0f) ? ang : 2 * M_PI_F + ang;
    return MapManager::NormalizeOrientation(ang);
}

bool WorldObject::HasInArc(float const arcangle, WorldObject const* obj) const
{
    // always have self in arc
    if (obj == this)
        return true;

    float arc = arcangle;

    // move arc to range 0.. 2*pi
    arc = MapManager::NormalizeOrientation(arc);

    float angle = GetAngle(obj);
    angle -= m_position.o;

    // move angle to range -pi ... +pi
    angle = MapManager::NormalizeOrientation(angle);
    if (angle > M_PI_F)
        angle -= 2.0f * M_PI_F;

    float lborder =  -1.0f * (arc / 2.0f);                    // in range -pi..0
    float rborder = (arc / 2.0f);                             // in range 0..pi
    return ((angle >= lborder) && (angle <= rborder));
}

bool WorldObject::isInFrontInMap(WorldObject const* target, float distance,  float arc) const
{
    return IsWithinDistInMap(target, distance) && HasInArc( arc, target );
}

bool WorldObject::isInBackInMap(WorldObject const* target, float distance, float arc) const
{
    return IsWithinDistInMap(target, distance) && !HasInArc( 2 * M_PI_F - arc, target );
}

bool WorldObject::isInFront(WorldObject const* target, float distance,  float arc) const
{
    return IsWithinDist(target, distance) && HasInArc( arc, target );
}

bool WorldObject::isInBack(WorldObject const* target, float distance, float arc) const
{
    return IsWithinDist(target, distance) && !HasInArc( 2 * M_PI_F - arc, target );
}

void WorldObject::GetRandomPoint(float x, float y, float z, float distance, float& rand_x, float& rand_y, float& rand_z, float minDist /*=0.0f*/, float const* ori /*=nullptr*/) const
{
    if (distance == 0)
    {
        rand_x = x;
        rand_y = y;
        rand_z = z;
        return;
    }

    // angle to face `obj` to `this`
    float angle;
    if (!ori)
        angle = rand_norm_f() * 2 * M_PI_F;
    else
        angle = *ori;

    float new_dist;
    if (minDist == 0.0f)
        new_dist = rand_norm_f() * distance;
    else
        new_dist = minDist + rand_norm_f() * (distance - minDist);

    rand_x = x + new_dist * cos(angle);
    rand_y = y + new_dist * sin(angle);
    rand_z = z;

    MaNGOS::NormalizeMapCoord(rand_x);
    MaNGOS::NormalizeMapCoord(rand_y);
    UpdateGroundPositionZ(rand_x, rand_y, rand_z);          // update to LOS height if available
}

void WorldObject::UpdateGroundPositionZ(float x, float y, float &z) const
{
    float new_z = GetMap()->GetHeight(GetPhaseMask(), x, y, z);
    if (new_z > INVALID_HEIGHT)
        z = new_z + 0.05f;                                   // just to be sure that we are not a few pixel under the surface
}

void WorldObject::UpdateAllowedPositionZ(float x, float y, float& z, Map* atMap /*=nullptr*/) const
{
    if (!atMap)
        atMap = GetMap();

    switch (GetTypeId())
    {
    case TYPEID_UNIT:
    {
        if (Unit* pVictim = ((Creature const*)this)->getVictim())
        {
            // anyway creature move to victim for thinly Z distance (shun some VMAP wrong ground calculating)
            if (fabs(GetPositionZ() - pVictim->GetPositionZ()) < 5.0f)
                return;
        }

        if (((Creature const*)this)->IsLevitating())
        {
            float ground_z = atMap->GetHeight(GetPhaseMask(), x, y, z);
            z = ground_z + ((Creature const*)this)->GetFloatValue(UNIT_FIELD_HOVERHEIGHT) + GetObjectBoundingRadius() * GetObjectScale();
        }
        // non fly unit don't must be in air
        // non swim unit must be at ground (mostly speedup, because it don't must be in water and water level check less fast
        else if (!((Creature*)this)->CanFly())
        {
            bool canSwim = ((Creature*)this)->CanSwim();
            float ground_z = z;
            float max_z = canSwim
                ? atMap->GetTerrain()->GetWaterOrGroundLevel(x, y, z, &ground_z, !((Unit const*)this)->HasAuraType(SPELL_AURA_WATER_WALK))
                : ((ground_z = atMap->GetHeight(GetPhaseMask(), x, y, z)));
            if (max_z > INVALID_HEIGHT)
            {
                if (z > max_z)
                    z = max_z;
                else if (z < ground_z)
                    z = ground_z;
            }
        }
        else
        {
            float ground_z = atMap->GetHeight(GetPhaseMask(), x, y, z);
            if (z < ground_z)
                z = ground_z;
        }
        break;
    }
    case TYPEID_PLAYER:
    {
        // for server controlled moves player work same as creature (but it can always swim)
        if (!((Player const*)this)->CanFly())
        {
            float ground_z = z;
            float max_z = GetTerrain()->GetWaterOrGroundLevel(x, y, z, &ground_z, !((Unit const*)this)->HasAuraType(SPELL_AURA_WATER_WALK));
            if (max_z > INVALID_HEIGHT)
            {
                if (max_z != ground_z && z > max_z)
                    z = max_z;
                else if (z < ground_z)
                    z = ground_z;
            }
        }
        else
        {
            float ground_z = atMap->GetHeight(GetPhaseMask(), x, y, z);
            if (z < ground_z)
                z = ground_z;
        }
        break;
    }
    default:
    {
        float ground_z = atMap->GetHeight(GetPhaseMask(), x, y, z);
        if (ground_z > INVALID_HEIGHT)
            z = ground_z;
        break;
    }
    }
}

bool WorldObject::IsPositionValid() const
{
    return MaNGOS::IsValidMapCoord(m_position.x,m_position.y,m_position.z,m_position.o);
}

void WorldObject::MonsterSay(const char* text, uint32 /*language*/, Unit const* target) const
{
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_MONSTER_SAY, text, LANG_UNIVERSAL, CHAT_TAG_NONE, GetObjectGuid(), GetName(),
        target ? target->GetObjectGuid() : ObjectGuid(),target ? target->GetName() : "");
    SendMessageToSetInRange(&data, sWorld.getConfig(CONFIG_FLOAT_LISTEN_RANGE_SAY), true);
}

void WorldObject::MonsterYell(const char* text, uint32 /*language*/, Unit const* target) const
{
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_MONSTER_YELL, text, LANG_UNIVERSAL, CHAT_TAG_NONE, GetObjectGuid(), GetName(),
        target ? target->GetObjectGuid() : ObjectGuid(),target ? target->GetName() : "");
    SendMessageToSetInRange(&data, sWorld.getConfig(CONFIG_FLOAT_LISTEN_RANGE_YELL), true);
}

void WorldObject::MonsterTextEmote(const char* text, Unit const* target, bool IsBossEmote) const
{
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, IsBossEmote ? CHAT_MSG_RAID_BOSS_EMOTE : CHAT_MSG_MONSTER_EMOTE, text, LANG_UNIVERSAL, CHAT_TAG_NONE, GetObjectGuid(), GetName(),
        target ? target->GetObjectGuid() : ObjectGuid(),target ? target->GetName() : "");
    SendMessageToSetInRange(&data, sWorld.getConfig(IsBossEmote ? CONFIG_FLOAT_LISTEN_RANGE_YELL : CONFIG_FLOAT_LISTEN_RANGE_TEXTEMOTE), true);
}

void WorldObject::MonsterWhisper(const char* text, Unit const* target, bool IsBossWhisper) const
{
    if (!target || target->GetTypeId() != TYPEID_PLAYER)
        return;

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, IsBossWhisper ? CHAT_MSG_RAID_BOSS_WHISPER : CHAT_MSG_MONSTER_WHISPER, text, LANG_UNIVERSAL, CHAT_TAG_NONE, GetObjectGuid(), GetName(),
        target->GetObjectGuid(), target->GetName());
    ((Player*)target)->GetSession()->SendPacket(&data);
}

namespace MaNGOS
{
    class MonsterChatBuilder
    {
        public:
            MonsterChatBuilder(WorldObject const& obj, ChatMsg msgtype, MangosStringLocale const* textData, Language language, Unit const* target)
                : i_object(obj), i_msgtype(msgtype), i_textData(textData), i_language(language), i_target(target) {}
            void operator()(WorldPacket& data, int32 loc_idx)
            {
                char const* text = nullptr;
                if ((int32)i_textData->Content.size() > loc_idx + 1 && !i_textData->Content[loc_idx + 1].empty())
                    text = i_textData->Content[loc_idx + 1].c_str();
                else
                    text = i_textData->Content[0].c_str();

                ChatHandler::BuildChatPacket(data, i_msgtype, text, i_language, CHAT_TAG_NONE, i_object.GetObjectGuid(), i_object.GetNameForLocaleIdx(loc_idx),
                    i_target ? i_target->GetObjectGuid() : ObjectGuid(), i_target ? i_target->GetNameForLocaleIdx(loc_idx) : "");
            }

        private:
            WorldObject const& i_object;
            ChatMsg i_msgtype;
            MangosStringLocale const* i_textData;
            Language i_language;
            Unit const* i_target;
    };
}                                                           // namespace MaNGOS

/// Helper function to create localized around a source
void _DoLocalizedTextAround(WorldObject const* source, MangosStringLocale const* textData, ChatMsg msgtype, Language language, Unit const* target, float range)
{
    MaNGOS::MonsterChatBuilder say_build(*source, msgtype, textData, language, target);
    MaNGOS::LocalizedPacketDo<MaNGOS::MonsterChatBuilder> say_do(say_build);
    MaNGOS::CameraDistWorker<MaNGOS::LocalizedPacketDo<MaNGOS::MonsterChatBuilder> > say_worker(source, range, say_do);
    Cell::VisitWorldObjects(source, say_worker, range);
}

/// Function that sends a text associated to a MangosString
void WorldObject::MonsterText(MangosStringLocale const* textData, Unit const* target) const
{
    MANGOS_ASSERT(textData);

    switch (textData->Type)
    {
        case CHAT_TYPE_SAY:
            _DoLocalizedTextAround(this, textData, CHAT_MSG_MONSTER_SAY, textData->LanguageId, target, sWorld.getConfig(CONFIG_FLOAT_LISTEN_RANGE_SAY));
            break;
        case CHAT_TYPE_YELL:
            _DoLocalizedTextAround(this, textData, CHAT_MSG_MONSTER_YELL, textData->LanguageId, target, sWorld.getConfig(CONFIG_FLOAT_LISTEN_RANGE_YELL));
            break;
        case CHAT_TYPE_TEXT_EMOTE:
            _DoLocalizedTextAround(this, textData, CHAT_MSG_MONSTER_EMOTE, LANG_UNIVERSAL, target, sWorld.getConfig(CONFIG_FLOAT_LISTEN_RANGE_TEXTEMOTE));
            break;
        case CHAT_TYPE_BOSS_EMOTE:
            _DoLocalizedTextAround(this, textData, CHAT_MSG_RAID_BOSS_EMOTE, LANG_UNIVERSAL, target, sWorld.getConfig(CONFIG_FLOAT_LISTEN_RANGE_YELL));
            break;
        case CHAT_TYPE_WHISPER:
        {
            if (!target || target->GetTypeId() != TYPEID_PLAYER)
                return;
            MaNGOS::MonsterChatBuilder say_build(*this, CHAT_MSG_MONSTER_WHISPER, textData, LANG_UNIVERSAL, target);
            MaNGOS::LocalizedPacketDo<MaNGOS::MonsterChatBuilder> say_do(say_build);
            say_do((Player*)target);
            break;
        }
        case CHAT_TYPE_BOSS_WHISPER:
        {
            if (!target || target->GetTypeId() != TYPEID_PLAYER)
                return;
            MaNGOS::MonsterChatBuilder say_build(*this, CHAT_MSG_RAID_BOSS_WHISPER, textData, LANG_UNIVERSAL, target);
            MaNGOS::LocalizedPacketDo<MaNGOS::MonsterChatBuilder> say_do(say_build);
            say_do((Player*)target);
            break;
        }
        case CHAT_TYPE_ZONE_YELL:
        {
            MaNGOS::MonsterChatBuilder say_build(*this, CHAT_MSG_MONSTER_YELL, textData, textData->LanguageId, target);
            MaNGOS::LocalizedPacketDo<MaNGOS::MonsterChatBuilder> say_do(say_build);
            uint32 zoneid = GetZoneId();
            Map::PlayerList const& pList = GetMap()->GetPlayers();
            for (Map::PlayerList::const_iterator itr = pList.begin(); itr != pList.end(); ++itr)
                if (itr->getSource()->GetZoneId() == zoneid)
                    say_do(itr->getSource());
            break;
        }
    }
}

void WorldObject::SendMessageToSet(WorldPacket* data, bool /*bToSelf*/) const
{
    //if object is in world, map for it already created!
    if (IsInWorld())
        GetMap()->MessageBroadcast(this, data);
}

void WorldObject::SendMessageToSetInRange(WorldPacket* data, float dist, bool /*bToSelf*/) const
{
    //if object is in world, map for it already created!
    if (IsInWorld())
        GetMap()->MessageDistBroadcast(this, data, dist);
}

void WorldObject::SendMessageToSetExcept(WorldPacket* data, Player const* skipped_receiver) const
{
    //if object is in world, map for it already created!
    if (IsInWorld())
    {
        MaNGOS::MessageDelivererExcept notifier(this, data, skipped_receiver);
        Cell::VisitWorldObjects(this, notifier, GetMap()->GetVisibilityDistance(this));
    }
}

void WorldObject::SendObjectDeSpawnAnim(ObjectGuid guid)
{
    WorldPacket data(SMSG_GAMEOBJECT_DESPAWN_ANIM, 8);
    data << guid;
    SendMessageToSet(&data, true);
}

void WorldObject::SendGameObjectCustomAnim(ObjectGuid guid, uint32 animId /*= 0*/)
{
    WorldPacket data(SMSG_GAMEOBJECT_CUSTOM_ANIM, 8+4);
    data << guid;
    data << uint32(animId);
    SendMessageToSet(&data, true);
}

void WorldObject::SetMap(Map* map)
{
    MANGOS_ASSERT(map);
    m_currMap = map;
    //lets save current map's Id/instanceId
    m_position.SetMapId(map->GetId());
    m_position.SetInstanceId(map->GetInstanceId());
}

TerrainInfo const* WorldObject::GetTerrain() const
{
    MANGOS_ASSERT(m_currMap);
    return m_currMap->GetTerrain();
}

void WorldObject::AddObjectToRemoveList()
{
    GetMap()->AddObjectToRemoveList(this);
}

void WorldObject::RemoveObjectFromRemoveList()
{
    GetMap()->RemoveObjectFromRemoveList(this);
}

Creature* WorldObject::SummonCreature(uint32 id, float x, float y, float z, float ang,TempSummonType spwtype,uint32 despwtime, bool asActiveObject)
{
    CreatureInfo const *cinfo = ObjectMgr::GetCreatureTemplate(id);
    if(!cinfo)
    {
        sLog.outErrorDb("WorldObject::SummonCreature: Creature (Entry: %u) not existed for summoner: %s. ", id, GetGuidStr().c_str());
        return nullptr;
    }

    TemporarySummon* pCreature = new TemporarySummon(GetObjectGuid());

    Team team = TEAM_NONE;
    if (GetTypeId()==TYPEID_PLAYER)
        team = ((Player*)this)->GetTeam();

    CreatureCreatePos pos(GetMap(), x, y, z, ang, GetPhaseMask());

    if (fabs(x) < M_NULL_F && fabs(y) < M_NULL_F && fabs(z) < M_NULL_F)
        pos = CreatureCreatePos(this, GetOrientation(), CONTACT_DISTANCE, ang);

    if (!pCreature->Create(GetMap()->GenerateLocalLowGuid(cinfo->GetHighGuid()), pos, cinfo, team))
    {
        delete pCreature;
        return nullptr;
    }

    pCreature->SetSummonPoint(pos);

    // Active state set before added to map
    pCreature->SetActiveObjectState(asActiveObject);

    pCreature->Summon(spwtype, despwtime);                  // Also initializes the AI and MMGen

    if (GetTypeId()==TYPEID_UNIT && ((Creature*)this)->AI())
        ((Creature*)this)->AI()->JustSummoned(pCreature);

    // Creature Linking, Initial load is handled like respawn
    if (pCreature->IsLinkingEventTrigger())
        GetMap()->GetCreatureLinkingHolder()->DoCreatureLinkingEvent(LINKING_EVENT_RESPAWN, pCreature);

    // return the creature therewith the summoner has access to it
    return pCreature;
}

GameObject* WorldObject::SummonGameobject(uint32 id, float x, float y, float z, float angle, uint32 despwtime)
{
    GameObject* pGameObj = new GameObject;

    Map *map = GetMap();

    if (!map)
        return nullptr;

    if(!pGameObj->Create(map->GenerateLocalLowGuid(HIGHGUID_GAMEOBJECT), id, map,
        GetPhaseMask(), x, y, z, angle))
    {
        delete pGameObj;
        return nullptr;
    }

    pGameObj->SetRespawnTime(despwtime/IN_MILLISECONDS);

    map->Add(pGameObj);

    return pGameObj;
}

// how much space should be left in front of/ behind a mob that already uses a space
#define OCCUPY_POS_DEPTH_FACTOR                          1.8f

namespace MaNGOS
{
    class NearUsedPosDo
    {
        public:
            NearUsedPosDo(WorldObject const& obj, WorldObject const* searcher, float absAngle, ObjectPosSelector& selector)
                : i_object(obj), i_searcher(searcher), i_absAngle(MapManager::NormalizeOrientation(absAngle)), i_selector(selector) {}

            void operator()(Corpse*) const {}
            void operator()(DynamicObject*) const {}

            void operator()(Creature* c) const
            {
                // skip self or target
                if (c == i_searcher || c == &i_object)
                    return;

                float x, y, z;

                if (c->IsStopped() || !c->GetMotionMaster()->GetDestination(x, y, z))
                {
                    x = c->GetPositionX();
                    y = c->GetPositionY();
                }

                add(c,x,y);
            }

            template<class T>
            void operator()(T* u) const
            {
                // skip self or target
                if (u == i_searcher || u == &i_object)
                    return;

                float x,y;

                x = u->GetPositionX();
                y = u->GetPositionY();

                add(u,x,y);
            }

            // we must add used pos that can fill places around center
            void add(WorldObject* u, float x, float y) const
            {
                float dx = i_object.GetPositionX() - x;
                float dy = i_object.GetPositionY() - y;
                float dist2d = sqrt((dx * dx) + (dy * dy));

                // It is ok for the objects to require a bit more space
                float delta = u->GetObjectBoundingRadius();
                if (i_selector.m_searchPosFor && i_selector.m_searchPosFor != u)
                    delta += i_selector.m_searchPosFor->GetObjectBoundingRadius();

                delta *= OCCUPY_POS_DEPTH_FACTOR;           // Increase by factor

                // u is too near/far away from i_object. Do not consider it to occupy space
                if (fabs(i_selector.m_searcherDist - dist2d) > delta)
                    return;

                float angle = i_object.GetAngle(u) - i_absAngle;

                // move angle to range -pi ... +pi, range before is -2Pi..2Pi
                if (angle > M_PI_F)
                    angle -= 2.0f * M_PI_F;
                else if (angle < -M_PI_F)
                    angle += 2.0f * M_PI_F;

                i_selector.AddUsedArea(u, angle, dist2d);
            }
        private:
            WorldObject const& i_object;
            WorldObject const* i_searcher;
            float              i_absAngle;
            ObjectPosSelector& i_selector;
    };
}                                                           // namespace MaNGOS

//===================================================================================================

void WorldObject::PlayMusic(uint32 sound_id, Player const* target /*= nullptr*/) const
{
    WorldPacket data(SMSG_PLAY_MUSIC, 4);
    data << uint32(sound_id);
    if (target)
        target->SendDirectMessage(&data);
    else
        SendMessageToSet(&data, true);
}

void WorldObject::GetNearPoint2D(float &x, float &y, float distance2d, float absAngle ) const
{
    x = GetPositionX() + distance2d * cos(absAngle);
    y = GetPositionY() + distance2d * sin(absAngle);

    MaNGOS::NormalizeMapCoord(x);
    MaNGOS::NormalizeMapCoord(y);
}

void WorldObject::GetNearPoint(WorldObject const* searcher, float &x, float &y, float &z, float searcher_bounding_radius, float distance2d, float absAngle) const
{
    GetNearPoint2D(x, y, distance2d, absAngle);
    const float init_z = z = GetPositionZ();

    // if detection disabled, return first point
    if(!sWorld.getConfig(CONFIG_BOOL_DETECT_POS_COLLISION))
    {
        if (searcher)
            searcher->UpdateAllowedPositionZ(x, y, z);      // update to LOS height if available
        else
            UpdateGroundPositionZ(x, y, z);
        return;
    }

    // or remember first point
    float first_x = x;
    float first_y = y;
    bool first_los_conflict = false;                        // first point LOS problems

    const float dist = distance2d + searcher_bounding_radius + GetObjectBoundingRadius();

    // prepare selector for work
    ObjectPosSelector selector(GetPositionX(), GetPositionY(), distance2d, searcher_bounding_radius, searcher);

    // adding used positions around object
    {
        MaNGOS::NearUsedPosDo u_do(*this, searcher, absAngle, selector);
        MaNGOS::WorldObjectWorker<MaNGOS::NearUsedPosDo> worker(this, u_do);

        Cell::VisitAllObjects(this, worker, dist);
    }

    // maybe can just place in primary position
    if (selector.CheckOriginalAngle())
    {
        if (searcher)
            searcher->UpdateAllowedPositionZ(x, y, z);      // update to LOS height if available
        else
            UpdateGroundPositionZ(x, y, z);

        if (fabs(init_z - z) < dist && IsWithinLOS(x, y, z))
            return;

        first_los_conflict = true;                          // first point have LOS problems
    }

    // set first used pos in lists
    selector.InitializeAngle();

    float angle;                                            // candidate of angle for free pos

    // select in positions after current nodes (selection one by one)
    while (selector.NextAngle(angle))                        // angle for free pos
    {
        GetNearPoint2D(x, y, distance2d, absAngle + angle);
        z = GetPositionZ();

        if (searcher)
            searcher->UpdateAllowedPositionZ(x, y, z);      // update to LOS height if available
        else
            UpdateGroundPositionZ(x, y, z);

        if (fabs(init_z - z) < dist && IsWithinLOS(x, y, z))
            return;
    }

    // BAD NEWS: not free pos (or used or have LOS problems)
    // Attempt find _used_ pos without LOS problem
    if (!first_los_conflict)
    {
        x = first_x;
        y = first_y;

        if (searcher)
            searcher->UpdateAllowedPositionZ(x, y, z);      // update to LOS height if available
        else
            UpdateGroundPositionZ(x, y, z);
        return;
    }

    // set first used pos in lists
    selector.InitializeAngle();

    // select in positions after current nodes (selection one by one)
    while (selector.NextUsedAngle(angle))                   // angle for used pos but maybe without LOS problem
    {
        GetNearPoint2D(x, y, distance2d, absAngle + angle);
        z = GetPositionZ();

        if (searcher)
            searcher->UpdateAllowedPositionZ(x, y, z);      // update to LOS height if available
        else
            UpdateGroundPositionZ(x, y, z);

        if (fabs(init_z - z) < dist && IsWithinLOS(x, y, z))
            return;
    }

    // BAD BAD NEWS: all found pos (free and used) have LOS problem :(
    x = first_x;
    y = first_y;

    if (searcher)
        searcher->UpdateAllowedPositionZ(x, y, z);          // update to LOS height if available
    else
        UpdateGroundPositionZ(x, y, z);
}

void WorldObject::SetPhaseMask(uint32 newPhaseMask, bool update)
{
    m_position.SetPhaseMask(newPhaseMask);

    if (update && IsInWorld())
        UpdateVisibilityAndView();
}

void WorldObject::PlayDistanceSound(uint32 sound_id, Player const* target /*= nullptr*/) const
{
    WorldPacket data(SMSG_PLAY_OBJECT_SOUND,4+8);
    data << uint32(sound_id);
    data << GetObjectGuid();
    if (target)
        target->SendDirectMessage( &data );
    else
        SendMessageToSet( &data, true );
}

void WorldObject::PlayDirectSound(uint32 sound_id, Player const* target /*= nullptr*/) const
{
    WorldPacket data(SMSG_PLAY_SOUND, 4);
    data << uint32(sound_id);
    if (target)
        target->SendDirectMessage( &data );
    else
        SendMessageToSet( &data, true );
}

GameObject* WorldObject::GetClosestGameObjectWithEntry(const WorldObject* pSource, uint32 uiEntry, float fMaxSearchRange)
{
    //return closest gameobject in grid, with range from pSource
    GameObject* pGameObject = nullptr;

    CellPair p(MaNGOS::ComputeCellPair(pSource->GetPositionX(), pSource->GetPositionY()));
    Cell cell(p);
    cell.SetNoCreate();

    MaNGOS::NearestGameObjectEntryInObjectRangeCheck gobject_check(*pSource, uiEntry, fMaxSearchRange);
    MaNGOS::GameObjectLastSearcher<MaNGOS::NearestGameObjectEntryInObjectRangeCheck> searcher(pGameObject, gobject_check);

    TypeContainerVisitor<MaNGOS::GameObjectLastSearcher<MaNGOS::NearestGameObjectEntryInObjectRangeCheck>, GridTypeMapContainer> grid_gobject_searcher(searcher);

    cell.Visit(p, grid_gobject_searcher,*(pSource->GetMap()), *this, fMaxSearchRange);

    return pGameObject;
}

void WorldObject::GetGameObjectListWithEntryInGrid(std::list<GameObject*>& lList, uint32 uiEntry, float fMaxSearchRange)
{
    CellPair pair(MaNGOS::ComputeCellPair(this->GetPositionX(), this->GetPositionY()));
    Cell cell(pair);
    cell.SetNoCreate();

    MaNGOS::AllGameObjectsWithEntryInRange check(this, uiEntry, fMaxSearchRange);
    MaNGOS::GameObjectListSearcher<MaNGOS::AllGameObjectsWithEntryInRange> searcher(lList, check);
    TypeContainerVisitor<MaNGOS::GameObjectListSearcher<MaNGOS::AllGameObjectsWithEntryInRange>, GridTypeMapContainer> visitor(searcher);

    GetMap()->Visit(cell, visitor);
}

void WorldObject::UpdateVisibilityAndView()
{
    GetViewPoint().Call_UpdateVisibilityForOwner();
    UpdateObjectVisibility();
    GetViewPoint().Event_ViewPointVisibilityChanged();
}

Creature* WorldObject::GetClosestCreatureWithEntry(WorldObject* pSource, uint32 uiEntry, float fMaxSearchRange)
{
   Creature *p_Creature = nullptr;
   CellPair p(MaNGOS::ComputeCellPair(pSource->GetPositionX(), pSource->GetPositionY()));

   Cell cell(p);
   cell.SetNoCreate();
   MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck u_check(*pSource,uiEntry,true,false,fMaxSearchRange);
   MaNGOS::CreatureLastSearcher<MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck> searcher(p_Creature, u_check);
   TypeContainerVisitor<MaNGOS::CreatureLastSearcher<MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck>, GridTypeMapContainer >  grid_creature_searcher(searcher);
   cell.Visit(p, grid_creature_searcher, *pSource->GetMap(), *this, fMaxSearchRange);
   return p_Creature;
}

void WorldObject::UpdateObjectVisibility()
{
    CellPair p = MaNGOS::ComputeCellPair(GetPositionX(), GetPositionY());
    Cell cell(p);

    GetMap()->UpdateObjectVisibility(this, cell, p);
}

void WorldObject::AddToClientUpdateList()
{
    GetMap()->AddUpdateObject(GetObjectGuid());
}

void WorldObject::RemoveFromClientUpdateList()
{
    GetMap()->RemoveUpdateObject(GetObjectGuid());
}

struct WorldObjectChangeAccumulator
{
    UpdateDataMapType &i_updateDatas;
    WorldObject &i_object;
    WorldObjectChangeAccumulator(WorldObject &obj, UpdateDataMapType &d) : i_updateDatas(d), i_object(obj)
    {
        // send self fields changes in another way, otherwise
        // with new camera system when player's camera too far from player, camera wouldn't receive packets and changes from player
        if (i_object.isType(TYPEMASK_PLAYER))
            i_object.BuildUpdateDataForPlayer((Player*)&i_object, i_updateDatas);
    }

    void Visit(CameraMapType &m)
    {
        for(CameraMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
        {
            Player* owner = iter->getSource()->GetOwner();
            if (owner && owner != &i_object && owner->HaveAtClient(i_object.GetObjectGuid()))
                i_object.BuildUpdateDataForPlayer(owner, i_updateDatas);
        }
    }

    template<class SKIP> void Visit(GridRefManager<SKIP> &) {}
};

void WorldObject::BuildUpdateData( UpdateDataMapType & update_players)
{
    WorldObjectChangeAccumulator notifier(*this, update_players);
    Cell::VisitWorldObjects(this, notifier, GetMap()->GetVisibilityDistance(this));

    ClearUpdateMask(false);
}

bool WorldObject::IsControlledByPlayer() const
{
    switch (GetTypeId())
    {
        case TYPEID_GAMEOBJECT:
            return ((GameObject*)this)->GetOwnerGuid().IsPlayer();
        case TYPEID_UNIT:
        case TYPEID_PLAYER:
            return ((Unit*)this)->IsCharmerOrOwnerPlayerOrPlayerItself();
        case TYPEID_DYNAMICOBJECT:
            return ((DynamicObject*)this)->GetCasterGuid().IsPlayer();
        case TYPEID_CORPSE:
            return true;
        default:
            return false;
    }
}

// Frozen Mod
void Object::ForceValuesUpdateAtIndex(uint16 index)
{
    MANGOS_ASSERT(index < m_valuesCount || PrintIndexError(index, true));

    m_changedValues[index] = true; // makes server think the field changed

    MarkForClientUpdate();
}
// Frozen Mod

bool WorldObject::PrintCoordinatesError(float x, float y, float z, char const* descr) const
{
    sLog.outError("%s with invalid %s coordinates: mapid = %uu, x = %f, y = %f, z = %f", GetGuidStr().c_str(), descr, GetMapId(), x, y, z);
    return false;                                           // always false for continue assert fail
}

void WorldObject::SetActiveObjectState(bool active)
{
    if (m_isActiveObject == active || (isType(TYPEMASK_PLAYER) && !active))  // player shouldn't became inactive, never
        return;

    if (IsInWorld() && !isType(TYPEMASK_PLAYER))
        // player's update implemented in a different from other active worldobject's way
        // it's considired to use generic way in future
    {
        if (isActiveObject() && !active)
            GetMap()->RemoveFromActive(this);
        else if (!isActiveObject() && active)
            GetMap()->AddToActive(this);
    }
    m_isActiveObject = active;
}

void WorldObject::UpdateWorldState(uint32 state, uint32 value)
{
    if (GetMap())
        sWorldStateMgr.SetWorldStateValueFor(GetMap(), state, value);
}

uint32 WorldObject::GetWorldState(uint32 stateId)
{
    return sWorldStateMgr.GetWorldStateValueFor(this, stateId);
}

WorldObjectEventProcessor* WorldObject::GetEvents()
{
    return &m_Events;
}

void WorldObject::KillAllEvents(bool force)
{
    GetEvents()->KillAllEvents(force);
}

void WorldObject::AddEvent(BasicEvent* Event, uint64 e_time, bool set_addtime)
{
    if (set_addtime)
        GetEvents()->AddEvent(Event, GetEvents()->CalculateTime(e_time), set_addtime);
    else
        GetEvents()->AddEvent(Event, e_time, set_addtime);
}

void WorldObject::UpdateEvents(uint32 update_diff, uint32 /*time*/)
{
    GetEvents()->RenewEvents();
    GetEvents()->Update(update_diff);
}

Transport* WorldObject::GetTransport() const
{
    return IsOnTransport() ? sObjectMgr.GetTransportByGuid(GetTransportInfo()->GetTransportGuid()) : nullptr;
}

bool WorldObject::IsOnTransport() const
{
    return GetTransportInfo() && GetTransportInfo()->GetTransportGuid().IsMOTransport();
}

TransportBase* WorldObject::GetTransportBase()
{
    if (GetObjectGuid().IsMOTransport())
        return ((Transport*)this)->GetTransportBase();
    else if (GetObjectGuid().IsUnit())
        return ((Unit*)this)->GetTransportBase();

    return nullptr;
}

void WorldObject::UpdateHelper::Update(uint32 time_diff)
{
    if (m_obj.SkipUpdate())
    {
        m_obj.SkipUpdate(false);
        return;
    }
    uint32 realDiff = m_obj.m_updateTracker.timeElapsed();
    if (realDiff < sWorld.getConfig(CONFIG_UINT32_INTERVAL_MAPUPDATE)/2)
        return;

    m_obj.m_updateTracker.Reset();
    m_obj.Update(realDiff, time_diff);
    m_obj.SetLastUpdateTime();
}
