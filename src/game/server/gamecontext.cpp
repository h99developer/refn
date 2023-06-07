﻿/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <base/math.h>

#include <engine/shared/config.h>
#include <engine/shared/memheap.h>
#include <engine/map.h>

#include <generated/server_data.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <game/version.h>

#include "entities/character.h"
#include "entities/projectile.h"
#include "gamemodes/fng2.h"
#include "gamemodes/fng2solo.h"
#include "gamemodes/fng2boom.h"
#include "gamemodes/fng2boomsolo.h"

//other gametypes(for modding without changing original sources)
#include "gamecontext_additional_gametypes_includes.h"

#include "laser_text.h"

#include <vector>
#include <time.h>

#include "gamecontext.h"
#include "player.h"

enum
{
	RESET,
	NO_RESET
};

int CountBits(int64_t Flag)
{
	int RetCount = 0;
	for(size_t i = 0; i < 64; ++i)
	{
		if((Flag & ((int64_t)1ll << (int64_t)i)) != 0)
			++RetCount;
	}

	return RetCount;
}

int PositionOfNonZeroBit(int64_t Mask, int64_t Offset)
{
	for (int64_t n = Offset; n < 64; ++n)
	{
		if ((Mask & (1ll << n)) != 0)
		{
			return n;
		}
	}
	return -1;
}

void CGameContext::Construct(int Resetting)
{
	m_Resetting = 0;
	m_pServer = 0;

	m_FirstServerCommand = NULL;

	for(int i = 0; i < MAX_CLIENTS; i++)
		m_apPlayers[i] = 0;

	m_pController = 0;
	m_VoteCloseTime = 0;
	m_VoteCancelTime = 0;
	m_pVoteOptionFirst = 0;
	m_pVoteOptionLast = 0;
	m_NumVoteOptions = 0;
	m_LockTeams = 0;

	if(Resetting==NO_RESET)
		m_pVoteOptionHeap = new CHeap();
}

CGameContext::CGameContext(int Resetting)
{
	Construct(Resetting);
}

CGameContext::CGameContext()
{
	Construct(NO_RESET);
}

CGameContext::~CGameContext()
{
	for(int i = 0; i < MAX_CLIENTS; i++)
		delete m_apPlayers[i];
	if(!m_Resetting)
		delete m_pVoteOptionHeap;

	sServerCommand *pCMD = m_FirstServerCommand;
	while(pCMD)
	{
		sServerCommand *pThis = pCMD;
		pCMD = pCMD->m_NextCommand;
		delete pThis;
	}
}

void CGameContext::Clear()
{
	CHeap *pVoteOptionHeap = m_pVoteOptionHeap;
	CVoteOptionServer *pVoteOptionFirst = m_pVoteOptionFirst;
	CVoteOptionServer *pVoteOptionLast = m_pVoteOptionLast;
	int NumVoteOptions = m_NumVoteOptions;
	CTuningParams Tuning = m_Tuning;

	m_Resetting = true;
	this->~CGameContext();
	mem_zero(this, sizeof(*this));
	new (this) CGameContext(RESET);

	m_pVoteOptionHeap = pVoteOptionHeap;
	m_pVoteOptionFirst = pVoteOptionFirst;
	m_pVoteOptionLast = pVoteOptionLast;
	m_NumVoteOptions = NumVoteOptions;
	m_Tuning = Tuning;
}


class CCharacter *CGameContext::GetPlayerChar(int ClientID)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || !m_apPlayers[ClientID])
		return 0;
	return m_apPlayers[ClientID]->GetCharacter();
}

void CGameContext::MakeLaserTextPoints(vec2 pPos, int pOwner, int pPoints){
	char text[10];
	if(pPoints >= 0)
		str_format(text, 10, "+%d", pPoints);
	else
		str_format(text, 10, "%d", pPoints);
	pPos.y -= 20.0 * 2.5;
	new CLaserText(&m_World, pPos, pOwner, Server()->TickSpeed() * 3, text, (int)(str_length(text)));
}

void CGameContext::CreateDamageInd(vec2 Pos, float Angle, int Amount, int Team, int FromPlayerID)
{
	float a = 3 * 3.14159f / 2 + Angle;
	float s = a - pi / 3;
	float e = a + pi / 3;

	if(m_pController->IsTeamplay())
	{
		int64_t mask = 0;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (m_apPlayers[i] && m_apPlayers[i]->GetTeam() == Team) mask |= CmaskOne(i);
		}

		float f = mix(s, e, float(Amount/2 + 1) / float(Amount + 2));
		CNetEvent_Damage *pEvent = (CNetEvent_Damage *)m_Events.Create(NETEVENTTYPE_DAMAGE, sizeof(CNetEvent_Damage), mask);
		if (pEvent)
		{
			pEvent->m_X = (int)Pos.x;
			pEvent->m_Y = (int)Pos.y;
			pEvent->m_Angle = (int)(f*256.0f);
			pEvent->m_ArmorAmount = 0;
			pEvent->m_HealthAmount = Amount;
			pEvent->m_ClientID = FromPlayerID;
			pEvent->m_Self = false;
		}
	} else if(FromPlayerID != -1)
	{
		float f = mix(s, e, float(Amount/2 + 1) / float(Amount + 2));
		CNetEvent_Damage *pEvent = (CNetEvent_Damage *)m_Events.Create(NETEVENTTYPE_DAMAGE, sizeof(CNetEvent_Damage), CmaskOne(FromPlayerID));
		if (pEvent)
		{
			pEvent->m_X = (int)Pos.x;
			pEvent->m_Y = (int)Pos.y;
			pEvent->m_Angle = (int)(f*256.0f);
			pEvent->m_ArmorAmount = 0;
			pEvent->m_HealthAmount = Amount;
			pEvent->m_ClientID = FromPlayerID;
			pEvent->m_Self = false;
		}	
	}
}

void CGameContext::CreateSoundTeam(vec2 Pos, int Sound, int TeamID, int FromPlayerID)
{
	if (Sound < 0)
		return;

	//Only when teamplay is activated
	if (m_pController->IsTeamplay())
	{
		int64_t mask = 0;
		for (int i = 0; i < MAX_CLIENTS; i++)
		{
			if (m_apPlayers[i] && m_apPlayers[i]->GetTeam() == TeamID && (FromPlayerID != i)) mask |= CmaskOne(i);
		}

		// create a sound
		CNetEvent_SoundWorld *pEvent = (CNetEvent_SoundWorld *)m_Events.Create(NETEVENTTYPE_SOUNDWORLD, sizeof(CNetEvent_SoundWorld), mask);
		if (pEvent)
		{
			pEvent->m_X = (int)Pos.x;
			pEvent->m_Y = (int)Pos.y;
			pEvent->m_SoundID = Sound;
		}
	}
	//the player "causing" this events gets a global sound
	if (FromPlayerID != -1)
		CreateSoundGlobal(Sound, FromPlayerID);
}

void CGameContext::CreateSoundGlobal(int Sound, int Target)
{
	if (Sound < 0)
		return;

	CNetEvent_SoundWorld *pEvent = (CNetEvent_SoundWorld *)m_Events.Create(NETEVENTTYPE_SOUNDWORLD, sizeof(CNetEvent_SoundWorld), int64_t(1 << Target));
	pEvent->m_SoundID = Sound;
	int Flag = MSGFLAG_VITAL;
	if(Target != -1)
		Flag |= MSGFLAG_NORECORD;
	
	vec2 Pos;
	if(Target != -1 && m_apPlayers[Target]->GetCharacter())
		Pos = m_apPlayers[Target]->GetCharacter()->GetPos();
	pEvent->m_X = (int)Pos.x;
	pEvent->m_Y = (int)Pos.y;
}

void CGameContext::CreateHammerHit(vec2 Pos)
{
	// create the event
	CNetEvent_HammerHit *pEvent = (CNetEvent_HammerHit *)m_Events.Create(NETEVENTTYPE_HAMMERHIT, sizeof(CNetEvent_HammerHit));
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
	}
}


void CGameContext::CreateExplosion(vec2 Pos, int Owner, int Weapon, int MaxDamage)
{
	// create the event
	CNetEvent_Explosion *pEvent = (CNetEvent_Explosion *)m_Events.Create(NETEVENTTYPE_EXPLOSION, sizeof(CNetEvent_Explosion));
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
	}

	// deal damage
	CCharacter *apEnts[MAX_CLIENTS];
	float Radius = g_pData->m_Explosion.m_Radius;
	float InnerRadius = 48.0f;
	float MaxForce = g_pData->m_Explosion.m_MaxForce;
	int Num = m_World.FindEntities(Pos, Radius, (CEntity**)apEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
	for(int i = 0; i < Num; i++)
	{
		vec2 Diff = apEnts[i]->GetPos() - Pos;
		vec2 Force(0, MaxForce);
		float l = length(Diff);
		if(l)
			Force = normalize(Diff) * MaxForce;
		float Factor = 1 - clamp((l-InnerRadius)/(Radius-InnerRadius), 0.0f, 1.0f);
		if((int)(Factor * MaxDamage))
			apEnts[i]->TakeDamage(Force * Factor, Diff*-1, (int)(Factor * MaxDamage), Owner, Weapon);
	}
}

void CGameContext::CreatePlayerSpawn(vec2 Pos)
{
	// create the event
	CNetEvent_Spawn *ev = (CNetEvent_Spawn *)m_Events.Create(NETEVENTTYPE_SPAWN, sizeof(CNetEvent_Spawn));
	if(ev)
	{
		ev->m_X = (int)Pos.x;
		ev->m_Y = (int)Pos.y;
	}
}

void CGameContext::CreateDeath(vec2 Pos, int ClientID)
{
	// create the event
	CNetEvent_Death *pEvent = (CNetEvent_Death *)m_Events.Create(NETEVENTTYPE_DEATH, sizeof(CNetEvent_Death));
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
		pEvent->m_ClientID = ClientID;
	}
}

void CGameContext::CreateSound(vec2 Pos, int Sound, int64 Mask)
{
	if (Sound < 0)
		return;

	// create a sound
	CNetEvent_SoundWorld *pEvent = (CNetEvent_SoundWorld *)m_Events.Create(NETEVENTTYPE_SOUNDWORLD, sizeof(CNetEvent_SoundWorld), Mask);
	if(pEvent)
	{
		pEvent->m_X = (int)Pos.x;
		pEvent->m_Y = (int)Pos.y;
		pEvent->m_SoundID = Sound;
	}
}

void CGameContext::SendChatTarget(int To, const char *pText)
{
	CNetMsg_Sv_Chat Msg;
	Msg.m_Mode = CHAT_ALL;
	Msg.m_ClientID = -1;
	Msg.m_TargetID = -1;
	Msg.m_pMessage = pText;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, To);
}

void CGameContext::SendChat(int ChatterClientID, int Mode, int To, const char *pText)
{
	char aBuf[256];
	if(ChatterClientID >= 0 && ChatterClientID < MAX_CLIENTS)
		str_format(aBuf, sizeof(aBuf), "%d:%d:%s: %s", ChatterClientID, Mode, Server()->ClientName(ChatterClientID), pText);
	else
		str_format(aBuf, sizeof(aBuf), "*** %s", pText);

	char aBufMode[32];
	if(Mode == CHAT_WHISPER)
		str_copy(aBufMode, "whisper", sizeof(aBufMode));
	else if(Mode == CHAT_TEAM)
		str_copy(aBufMode, "teamchat", sizeof(aBufMode));
	else
		str_copy(aBufMode, "chat", sizeof(aBufMode));

	Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, aBufMode, aBuf);


	CNetMsg_Sv_Chat Msg;
	Msg.m_Mode = Mode;
	Msg.m_ClientID = ChatterClientID;
	Msg.m_pMessage = pText;
	Msg.m_TargetID = -1;

	if(Mode == CHAT_ALL)
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
	else if(Mode == CHAT_TEAM)
	{
		// pack one for the recording only
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL|MSGFLAG_NOSEND, -1);

		To = m_apPlayers[ChatterClientID]->GetTeam();

		// send to the clients
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(m_apPlayers[i] && m_apPlayers[i]->GetTeam() == To)
				Server()->SendPackMsg(&Msg, MSGFLAG_VITAL|MSGFLAG_NORECORD, i);
		}
	}
	else // Mode == CHAT_WHISPER
	{
		// send to the clients
		Msg.m_TargetID = To;
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ChatterClientID);
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, To);
	}
}

void CGameContext::SendBroadcast(const char* pText, int ClientID)
{
	CNetMsg_Sv_Broadcast Msg;
	Msg.m_pMessage = pText;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendEmoticon(int ClientID, int Emoticon)
{
	CNetMsg_Sv_Emoticon Msg;
	Msg.m_ClientID = ClientID;
	Msg.m_Emoticon = Emoticon;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
}

void CGameContext::SendWeaponPickup(int ClientID, int Weapon)
{
	CNetMsg_Sv_WeaponPickup Msg;
	Msg.m_Weapon = Weapon;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendMotd(int ClientID)
{
	CNetMsg_Sv_Motd Msg;
	Msg.m_pMessage = g_Config.m_SvMotd;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendSettings(int ClientID)
{
	CNetMsg_Sv_ServerSettings Msg;
	Msg.m_KickVote = g_Config.m_SvVoteKick;
	Msg.m_KickMin = g_Config.m_SvVoteKickMin;
	Msg.m_SpecVote = g_Config.m_SvVoteSpectate;
	Msg.m_TeamLock = m_LockTeams != 0;
	Msg.m_TeamBalance = g_Config.m_SvTeambalanceTime != 0;
	Msg.m_PlayerSlots = g_Config.m_SvPlayerSlots;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendSkinChange(int ClientID, int TargetID)
{
	CNetMsg_Sv_SkinChange Msg;
	Msg.m_ClientID = ClientID;
	for(int p = 0; p < NUM_SKINPARTS; p++)
	{
		Msg.m_apSkinPartNames[p] = m_apPlayers[ClientID]->m_TeeInfos.m_aaSkinPartNames[p];
		Msg.m_aUseCustomColors[p] = m_apPlayers[ClientID]->m_TeeInfos.m_aUseCustomColors[p];
		Msg.m_aSkinPartColors[p] = m_apPlayers[ClientID]->m_TeeInfos.m_aSkinPartColors[p];
	}
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL|MSGFLAG_NORECORD, TargetID);
}

void CGameContext::SendGameMsg(int GameMsgID, int ClientID)
{
	CMsgPacker Msg(NETMSGTYPE_SV_GAMEMSG);
	Msg.AddInt(GameMsgID);
	Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendGameMsg(int GameMsgID, int ParaI1, int ClientID)
{
	CMsgPacker Msg(NETMSGTYPE_SV_GAMEMSG);
	Msg.AddInt(GameMsgID);
	Msg.AddInt(ParaI1);
	Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SendGameMsg(int GameMsgID, int ParaI1, int ParaI2, int ParaI3, int ClientID)
{
	CMsgPacker Msg(NETMSGTYPE_SV_GAMEMSG);
	Msg.AddInt(GameMsgID);
	Msg.AddInt(ParaI1);
	Msg.AddInt(ParaI2);
	Msg.AddInt(ParaI3);
	Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

//
void CGameContext::StartVote(const char *pDesc, const char *pCommand, const char *pReason)
{
	// check if a vote is already running
	if(m_VoteCloseTime)
		return;

	// reset votes
	m_VoteEnforce = VOTE_ENFORCE_UNKNOWN;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
		{
			m_apPlayers[i]->m_Vote = 0;
			m_apPlayers[i]->m_VotePos = 0;
		}
	}

	// start vote
	m_VoteCloseTime = time_get() + time_freq()*VOTE_TIME;
	m_VoteCancelTime = time_get() + time_freq()*VOTE_CANCEL_TIME;
	str_copy(m_aVoteDescription, pDesc, sizeof(m_aVoteDescription));
	str_copy(m_aVoteCommand, pCommand, sizeof(m_aVoteCommand));
	str_copy(m_aVoteReason, pReason, sizeof(m_aVoteReason));
	SendVoteSet(m_VoteType, -1);
	m_VoteUpdate = true;
}


void CGameContext::EndVote(int Type, bool Force)
{
	m_VoteCloseTime = 0;
	m_VoteCancelTime = 0;
	if(Force)
		m_VoteCreator = -1;
	SendVoteSet(Type, -1);
}

void CGameContext::ForceVote(int Type, const char *pDescription, const char *pReason)
{
	CNetMsg_Sv_VoteSet Msg;
	Msg.m_Type = Type;
	Msg.m_Timeout = 0;
	Msg.m_ClientID = -1;
	Msg.m_pDescription = pDescription;
	Msg.m_pReason = pReason;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);
}

void CGameContext::SendVoteSet(int Type, int ToClientID)
{
	CNetMsg_Sv_VoteSet Msg;
	if(m_VoteCloseTime)
	{
		Msg.m_ClientID = m_VoteCreator;
		Msg.m_Type = Type;
		Msg.m_Timeout = (m_VoteCloseTime-time_get())/time_freq();
		Msg.m_pDescription = m_aVoteDescription;
		Msg.m_pReason = m_aVoteReason;
	}
	else
	{
		Msg.m_Type = Type;
		Msg.m_Timeout = 0;
		Msg.m_ClientID = m_VoteCreator;
		Msg.m_pDescription = "";
		Msg.m_pReason = "";
	}
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ToClientID);
}

void CGameContext::SendVoteStatus(int ClientID, int Total, int Yes, int No)
{
	CNetMsg_Sv_VoteStatus Msg = {0};
	Msg.m_Total = Total;
	Msg.m_Yes = Yes;
	Msg.m_No = No;
	Msg.m_Pass = Total - (Yes+No);

	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);

}

void CGameContext::AbortVoteOnDisconnect(int ClientID)
{
	if(m_VoteCloseTime && ClientID == m_VoteClientID && (str_startswith(m_aVoteCommand, "kick ") ||
		str_startswith(m_aVoteCommand, "set_team ") || (str_startswith(m_aVoteCommand, "ban ") && Server()->IsBanned(ClientID))))
		m_VoteCloseTime = -1;
}

void CGameContext::AbortVoteOnTeamChange(int ClientID)
{
	if(m_VoteCloseTime && ClientID == m_VoteClientID && str_startswith(m_aVoteCommand, "set_team "))
		m_VoteCloseTime = -1;
}


void CGameContext::CheckPureTuning()
{
	// might not be created yet during start up
	if(!m_pController)
		return;

	if(	str_comp(m_pController->GetGameType(), "DM")==0 ||
		str_comp(m_pController->GetGameType(), "TDM")==0 ||
		str_comp(m_pController->GetGameType(), "CTF")==0 ||
		str_comp(m_pController->GetGameType(), "LMS")==0 ||
		str_comp(m_pController->GetGameType(), "LTS")==0)
	{
		CTuningParams p;
		if(mem_comp(&p, &m_Tuning, sizeof(p)) != 0)
		{
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "resetting tuning due to pure server");
			m_Tuning = p;
		}
	}
}

void CGameContext::SendTuningParams(int ClientID)
{
	CheckPureTuning();

	CMsgPacker Msg(NETMSGTYPE_SV_TUNEPARAMS);
	int *pParams = (int *)&m_Tuning;
	for(unsigned i = 0; i < sizeof(m_Tuning)/sizeof(int); i++)
		Msg.AddInt(pParams[i]);
	Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}


//tune for frozen tees
void CGameContext::SendFakeTuningParams(int ClientID)
{
	static CTuningParams FakeTuning;
	
	FakeTuning.m_GroundControlSpeed = 0;
	FakeTuning.m_GroundJumpImpulse = 0;
	FakeTuning.m_GroundControlAccel = 0;
	FakeTuning.m_AirControlSpeed = 0;
	FakeTuning.m_AirJumpImpulse = 0;
	FakeTuning.m_AirControlAccel = 0;
	FakeTuning.m_HookDragSpeed = 0;
	FakeTuning.m_HookDragAccel = 0;
	FakeTuning.m_HookFireSpeed = 0;
	
	CMsgPacker Msg(NETMSGTYPE_SV_TUNEPARAMS);
	int *pParams = (int *)&FakeTuning;
	for(unsigned i = 0; i < sizeof(FakeTuning)/sizeof(int); i++)
		Msg.AddInt(pParams[i]);
	Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
}

void CGameContext::SwapTeams()
{
	if(!m_pController->IsTeamplay())
		return;

	SendGameMsg(GAMEMSG_TEAM_SWAP, -1);

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(m_apPlayers[i] && m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS)
			m_pController->DoTeamChange(m_apPlayers[i], m_apPlayers[i]->GetTeam()^1, false);
	}

	m_pController->SwapTeamscore();
}

void CGameContext::OnTick()
{
	// check tuning
	CheckPureTuning();

	// copy tuning
	m_World.m_Core.m_Tuning = m_Tuning;
	m_World.Tick();

	//if(world.paused) // make sure that the game object always updates
	m_pController->Tick();

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
		{
			m_apPlayers[i]->Tick();
			m_apPlayers[i]->PostTick();
		}
	}

	// update voting
	if(m_VoteCloseTime)
	{
		// abort the kick-vote on player-leave
		if(m_VoteCloseTime == -1)
			EndVote(VOTE_END_ABORT, false);
		else
		{
			int Total = 0, Yes = 0, No = 0;
			if(m_VoteUpdate)
			{
				// count votes
				char aaBuf[MAX_CLIENTS][NETADDR_MAXSTRSIZE] = {{0}};
				for(int i = 0; i < MAX_CLIENTS; i++)
					if(m_apPlayers[i])
						Server()->GetClientAddr(i, aaBuf[i], NETADDR_MAXSTRSIZE);
				bool aVoteChecked[MAX_CLIENTS] = {0};
				for(int i = 0; i < MAX_CLIENTS; i++)
				{
					if(!m_apPlayers[i] || m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS || aVoteChecked[i])	// don't count in votes by spectators
						continue;

					int ActVote = m_apPlayers[i]->m_Vote;
					int ActVotePos = m_apPlayers[i]->m_VotePos;

					// check for more players with the same ip (only use the vote of the one who voted first)
					for(int j = i+1; j < MAX_CLIENTS; ++j)
					{
						if(!m_apPlayers[j] || aVoteChecked[j] || str_comp(aaBuf[j], aaBuf[i]))
							continue;

						aVoteChecked[j] = true;
						if(m_apPlayers[j]->m_Vote && (!ActVote || ActVotePos > m_apPlayers[j]->m_VotePos))
						{
							ActVote = m_apPlayers[j]->m_Vote;
							ActVotePos = m_apPlayers[j]->m_VotePos;
						}
					}

					Total++;
					if(ActVote > 0)
						Yes++;
					else if(ActVote < 0)
						No++;
				}
			}

			if(m_VoteEnforce == VOTE_ENFORCE_YES || (m_VoteUpdate && Yes >= Total/2+1))
			{
				Server()->SetRconCID(IServer::RCON_CID_VOTE);
				Console()->ExecuteLine(m_aVoteCommand);
				Server()->SetRconCID(IServer::RCON_CID_SERV);
				if(m_VoteCreator != -1 && m_apPlayers[m_VoteCreator])
					m_apPlayers[m_VoteCreator]->m_LastVoteCall = 0;

				EndVote(VOTE_END_PASS, m_VoteEnforce==VOTE_ENFORCE_YES);
			}
			else if(m_VoteEnforce == VOTE_ENFORCE_NO || (m_VoteUpdate && No >= (Total+1)/2) || time_get() > m_VoteCloseTime)
				EndVote(VOTE_END_FAIL, m_VoteEnforce==VOTE_ENFORCE_NO);
			else if(m_VoteUpdate)
			{
				m_VoteUpdate = false;
				SendVoteStatus(-1, Total, Yes, No);
			}
		}
	}


#ifdef CONF_DEBUG
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i] && m_apPlayers[i]->IsDummy())
		{
			CNetObj_PlayerInput Input = {0};
			Input.m_Direction = (i&1)?-1:1;
			m_apPlayers[i]->OnPredictedInput(&Input);
		}
	}
#endif
}

// Server hooks
void CGameContext::OnClientDirectInput(int ClientID, void *pInput)
{
	int NumFailures = m_NetObjHandler.NumObjFailures();
	if(m_NetObjHandler.ValidateObj(NETOBJTYPE_PLAYERINPUT, pInput, sizeof(CNetObj_PlayerInput)) == -1)
	{
		if(g_Config.m_Debug && NumFailures != m_NetObjHandler.NumObjFailures())
		{
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "NETOBJTYPE_PLAYERINPUT failed on '%s'", m_NetObjHandler.FailedObjOn());
			Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
		}
	}
	else
		m_apPlayers[ClientID]->OnDirectInput((CNetObj_PlayerInput *)pInput);
}

void CGameContext::OnClientPredictedInput(int ClientID, void *pInput)
{
	if(!m_World.m_Paused)
	{
		int NumFailures = m_NetObjHandler.NumObjFailures();
		if(m_NetObjHandler.ValidateObj(NETOBJTYPE_PLAYERINPUT, pInput, sizeof(CNetObj_PlayerInput)) == -1)
		{
			if(g_Config.m_Debug && NumFailures != m_NetObjHandler.NumObjFailures())
			{
				char aBuf[128];
				str_format(aBuf, sizeof(aBuf), "NETOBJTYPE_PLAYERINPUT corrected on '%s'", m_NetObjHandler.FailedObjOn());
				Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
			}
		}
		else
			m_apPlayers[ClientID]->OnPredictedInput((CNetObj_PlayerInput *)pInput);
	}
}

void CGameContext::OnClientEnter(int ClientID)
{
	m_pController->OnPlayerConnect(m_apPlayers[ClientID]);

	SendPlayerCommands(ClientID);

	m_VoteUpdate = true;

	// update client infos (others before local)
	CNetMsg_Sv_ClientInfo NewClientInfoMsg;
	NewClientInfoMsg.m_ClientID = ClientID;
	NewClientInfoMsg.m_Local = 0;
	NewClientInfoMsg.m_Team = m_apPlayers[ClientID]->GetTeam();
	NewClientInfoMsg.m_pName = Server()->ClientName(ClientID);
	NewClientInfoMsg.m_pClan = Server()->ClientClan(ClientID);
	NewClientInfoMsg.m_Country = Server()->ClientCountry(ClientID);
	NewClientInfoMsg.m_Silent = false;

	if(g_Config.m_SvSilentSpectatorMode && m_apPlayers[ClientID]->GetTeam() == TEAM_SPECTATORS)
		NewClientInfoMsg.m_Silent = true;

	for(int p = 0; p < NUM_SKINPARTS; p++)
	{
		NewClientInfoMsg.m_apSkinPartNames[p] = m_apPlayers[ClientID]->m_TeeInfos.m_aaSkinPartNames[p];
		NewClientInfoMsg.m_aUseCustomColors[p] = m_apPlayers[ClientID]->m_TeeInfos.m_aUseCustomColors[p];
		NewClientInfoMsg.m_aSkinPartColors[p] = m_apPlayers[ClientID]->m_TeeInfos.m_aSkinPartColors[p];
	}


	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(i == ClientID || !m_apPlayers[i] || (!Server()->ClientIngame(i) && !m_apPlayers[i]->IsDummy()))
			continue;

		// new info for others
		if(Server()->ClientIngame(i))
			Server()->SendPackMsg(&NewClientInfoMsg, MSGFLAG_VITAL|MSGFLAG_NORECORD, i);

		// existing infos for new player
		CNetMsg_Sv_ClientInfo ClientInfoMsg;
		ClientInfoMsg.m_ClientID = i;
		ClientInfoMsg.m_Local = 0;
		ClientInfoMsg.m_Team = m_apPlayers[i]->GetTeam();
		ClientInfoMsg.m_pName = Server()->ClientName(i);
		ClientInfoMsg.m_pClan = Server()->ClientClan(i);
		ClientInfoMsg.m_Country = Server()->ClientCountry(i);
		ClientInfoMsg.m_Silent = false;
		for(int p = 0; p < NUM_SKINPARTS; p++)
		{
			ClientInfoMsg.m_apSkinPartNames[p] = m_apPlayers[i]->m_TeeInfos.m_aaSkinPartNames[p];
			ClientInfoMsg.m_aUseCustomColors[p] = m_apPlayers[i]->m_TeeInfos.m_aUseCustomColors[p];
			ClientInfoMsg.m_aSkinPartColors[p] = m_apPlayers[i]->m_TeeInfos.m_aSkinPartColors[p];
		}
		Server()->SendPackMsg(&ClientInfoMsg, MSGFLAG_VITAL|MSGFLAG_NORECORD, ClientID);
	}

	// local info
	NewClientInfoMsg.m_Local = 1;
	Server()->SendPackMsg(&NewClientInfoMsg, MSGFLAG_VITAL|MSGFLAG_NORECORD, ClientID);

	if(Server()->DemoRecorder_IsRecording())
	{
		CNetMsg_De_ClientEnter Msg;
		Msg.m_pName = NewClientInfoMsg.m_pName;
		Msg.m_ClientID = ClientID;
		Msg.m_Team = NewClientInfoMsg.m_Team;
		Server()->SendPackMsg(&Msg, MSGFLAG_NOSEND, -1);
	}
}

void CGameContext::OnClientConnected(int ClientID, bool Dummy, bool AsSpec)
{
	if(m_apPlayers[ClientID])
	{
		dbg_assert(m_apPlayers[ClientID]->IsDummy(), "invalid clientID");
		OnClientDrop(ClientID, "removing dummy", true);
	}

	m_apPlayers[ClientID] = new(ClientID) CPlayer(this, ClientID, Dummy, AsSpec);

	if(Dummy)
		return;

	// send active vote
	if(m_VoteCloseTime)
		SendVoteSet(m_VoteType, ClientID);

	// send motd
	SendMotd(ClientID);

	// send settings
	SendSettings(ClientID);
}

void CGameContext::OnClientTeamChange(int ClientID)
{
	if(m_apPlayers[ClientID]->GetTeam() == TEAM_SPECTATORS)
		AbortVoteOnTeamChange(ClientID);

	// mark client's projectile has team projectile
	CProjectile *p = (CProjectile *)m_World.FindFirst(CGameWorld::ENTTYPE_PROJECTILE);
	for(; p; p = (CProjectile *)p->TypeNext())
	{
		if(p->GetOwner() == ClientID)
			p->LoseOwner();
	}
}

bool CGameContext::OnClientDrop(int ClientID, const char *pReason, bool Force)
{
	if (m_apPlayers[ClientID]->GetCharacter() && m_apPlayers[ClientID]->GetCharacter()->IsFrozen() && !m_pController->IsGameOver() && !Force)
		return false;

	AbortVoteOnDisconnect(ClientID);
	m_pController->OnPlayerDisconnect(m_apPlayers[ClientID]);

	// update clients on drop
	if(Server()->ClientIngame(ClientID))
	{
		if(Server()->DemoRecorder_IsRecording())
		{
			CNetMsg_De_ClientLeave Msg;
			Msg.m_pName = Server()->ClientName(ClientID);
			Msg.m_pReason = pReason;
			Server()->SendPackMsg(&Msg, MSGFLAG_NOSEND, -1);
		}

		CNetMsg_Sv_ClientDrop Msg;
		Msg.m_ClientID = ClientID;
		Msg.m_pReason = pReason;
		Msg.m_Silent = false;
		if(g_Config.m_SvSilentSpectatorMode && m_apPlayers[ClientID]->GetTeam() == TEAM_SPECTATORS)
			Msg.m_Silent = true;
		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL|MSGFLAG_NORECORD, -1);
	}

	// mark client's projectile has team projectile
	CProjectile *p = (CProjectile *)m_World.FindFirst(CGameWorld::ENTTYPE_PROJECTILE);
	for(; p; p = (CProjectile *)p->TypeNext())
	{
		if(p->GetOwner() == ClientID)
			p->LoseOwner();
	}

	delete m_apPlayers[ClientID];
	m_apPlayers[ClientID] = 0;

	m_VoteUpdate = true;

	return true;
}

void CGameContext::OnMessage(int MsgID, CUnpacker *pUnpacker, int ClientID)
{
	void *pRawMsg = m_NetObjHandler.SecureUnpackMsg(MsgID, pUnpacker);
	CPlayer *pPlayer = m_apPlayers[ClientID];

	if(!pRawMsg)
	{
		if(g_Config.m_Debug)
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "dropped weird message '%s' (%d), failed on '%s'", m_NetObjHandler.GetMsgName(MsgID), MsgID, m_NetObjHandler.FailedMsgOn());
			Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
		}
		return;
	}

	if(Server()->ClientIngame(ClientID))
	{
		if(MsgID == NETMSGTYPE_CL_SAY)
		{
			if(g_Config.m_SvSpamprotection && pPlayer->m_LastChat && pPlayer->m_LastChat+Server()->TickSpeed() > Server()->Tick())
				return;

			CNetMsg_Cl_Say *pMsg = (CNetMsg_Cl_Say *)pRawMsg;

			// trim right and set maximum length to 128 utf8-characters
			int Length = 0;
			const char *p = pMsg->m_pMessage;
			const char *pEnd = 0;
			while(*p)
			{
				const char *pStrOld = p;
				int Code = str_utf8_decode(&p);

				// check if unicode is not empty
				if(!str_utf8_is_whitespace(Code))
				{
					pEnd = 0;
				}
				else if(pEnd == 0)
					pEnd = pStrOld;

				if(++Length >= 127)
				{
					*(const_cast<char *>(p)) = 0;
					break;
				}
			}
			if(pEnd != 0)
				*(const_cast<char *>(pEnd)) = 0;

			// drop empty and autocreated spam messages (more than 20 characters per second)
			if(Length == 0 || (g_Config.m_SvSpamprotection && pPlayer->m_LastChat && pPlayer->m_LastChat + Server()->TickSpeed()*(Length/20) > Server()->Tick()))
				return;
			
			//fair spam protection(no mute system needed)
			if (pPlayer->m_LastChat + Server()->TickSpeed()*((15 + Length) / 16 + 1) > Server()->Tick()) {
				++pPlayer->m_ChatSpamCount;
				if (++pPlayer->m_ChatSpamCount >= 5) {
					//"muted" for 2 seconds
					pPlayer->m_LastChat = Server()->Tick() + Server()->TickSpeed() * 2;
					pPlayer->m_ChatSpamCount = 0;
				}
				else pPlayer->m_LastChat = Server()->Tick();
			}
			else {
				pPlayer->m_LastChat = Server()->Tick();
				pPlayer->m_ChatSpamCount = 0;
			}

			if (pMsg->m_pMessage[0] == '/' && Length > 1) {
				ExecuteServerCommand(ClientID, pMsg->m_pMessage + 1);
				return;
			}

			// don't allow spectators to disturb players during a running game in tournament mode
			int Mode = pMsg->m_Mode;
			if((g_Config.m_SvTournamentMode == 2) &&
				pPlayer->GetTeam() == TEAM_SPECTATORS &&
				m_pController->IsGameRunning() &&
				!Server()->IsAuthed(ClientID))
			{
				if(Mode != CHAT_WHISPER)
					Mode = CHAT_TEAM;
				else if(m_apPlayers[pMsg->m_Target] && m_apPlayers[pMsg->m_Target]->GetTeam() != TEAM_SPECTATORS)
					Mode = CHAT_NONE;
			}

			if(Mode != CHAT_NONE)
				SendChat(ClientID, Mode, pMsg->m_Target, pMsg->m_pMessage);
		}
		else if(MsgID == NETMSGTYPE_CL_CALLVOTE)
		{
			CNetMsg_Cl_CallVote *pMsg = (CNetMsg_Cl_CallVote *)pRawMsg;
			int64 Now = Server()->Tick();

			if(pMsg->m_Force)
			{
				if(!Server()->IsAuthed(ClientID))
					return;
			}
			else
			{
				if((g_Config.m_SvSpamprotection && ((pPlayer->m_LastVoteTry && pPlayer->m_LastVoteTry+Server()->TickSpeed()*3 > Now) ||
					(pPlayer->m_LastVoteCall && pPlayer->m_LastVoteCall+Server()->TickSpeed()*VOTE_COOLDOWN > Now))) ||
					pPlayer->GetTeam() == TEAM_SPECTATORS || m_VoteCloseTime)
					return;

				pPlayer->m_LastVoteTry = Now;
			}

			m_VoteType = VOTE_UNKNOWN;
			char aDesc[VOTE_DESC_LENGTH] = {0};
			char aCmd[VOTE_CMD_LENGTH] = {0};
			const char *pReason = pMsg->m_Reason[0] ? pMsg->m_Reason : "No reason given";

			if(str_comp_nocase(pMsg->m_Type, "option") == 0)
			{
				CVoteOptionServer *pOption = m_pVoteOptionFirst;
				while(pOption)
				{
					if(str_comp_nocase(pMsg->m_Value, pOption->m_aDescription) == 0)
					{
						str_format(aDesc, sizeof(aDesc), "%s", pOption->m_aDescription);
						str_format(aCmd, sizeof(aCmd), "%s", pOption->m_aCommand);
						char aBuf[128];
						str_format(aBuf, sizeof(aBuf),
							"'%d:%s' voted %s '%s' reason='%s' cmd='%s' force=%d",
							ClientID, Server()->ClientName(ClientID), pMsg->m_Type,
							aDesc, pReason, aCmd, pMsg->m_Force
						);
						Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
						if(pMsg->m_Force)
						{
							Server()->SetRconCID(ClientID);
							Console()->ExecuteLine(aCmd);
							Server()->SetRconCID(IServer::RCON_CID_SERV);
							ForceVote(VOTE_START_OP, aDesc, pReason);
							return;
						}
						m_VoteType = VOTE_START_OP;
						break;
					}

					pOption = pOption->m_pNext;
				}

				if(!pOption)
					return;
			}
			else if(str_comp_nocase(pMsg->m_Type, "kick") == 0)
			{
				if(!g_Config.m_SvVoteKick || m_pController->GetRealPlayerNum() < g_Config.m_SvVoteKickMin)
					return;

				int KickID = str_toint(pMsg->m_Value);
				if(KickID < 0 || KickID >= MAX_CLIENTS || !m_apPlayers[KickID] || KickID == ClientID || Server()->IsAuthed(KickID))
					return;

				str_format(aDesc, sizeof(aDesc), "%2d: %s", KickID, Server()->ClientName(KickID));
				if (!g_Config.m_SvVoteKickBantime)
					str_format(aCmd, sizeof(aCmd), "kick %d Kicked by vote", KickID);
				else
				{
					char aAddrStr[NETADDR_MAXSTRSIZE] = {0};
					Server()->GetClientAddr(KickID, aAddrStr, sizeof(aAddrStr));
					str_format(aCmd, sizeof(aCmd), "ban %s %d Banned by vote", aAddrStr, g_Config.m_SvVoteKickBantime);
				}
				char aBuf[128];
				str_format(aBuf, sizeof(aBuf),
					"'%d:%s' voted %s '%d:%s' reason='%s' cmd='%s' force=%d",
					ClientID, Server()->ClientName(ClientID), pMsg->m_Type,
					KickID, Server()->ClientName(KickID), pReason, aCmd, pMsg->m_Force
				);
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
				if(pMsg->m_Force)
				{
					Server()->SetRconCID(ClientID);
					Console()->ExecuteLine(aCmd);
					Server()->SetRconCID(IServer::RCON_CID_SERV);
					return;
				}
				m_VoteType = VOTE_START_KICK;
				m_VoteClientID = KickID;
			}
			else if(str_comp_nocase(pMsg->m_Type, "spectate") == 0)
			{
				if(!g_Config.m_SvVoteSpectate)
					return;

				int SpectateID = str_toint(pMsg->m_Value);
				if(SpectateID < 0 || SpectateID >= MAX_CLIENTS || !m_apPlayers[SpectateID] || m_apPlayers[SpectateID]->GetTeam() == TEAM_SPECTATORS || SpectateID == ClientID)
					return;

				str_format(aDesc, sizeof(aDesc), "%2d: %s", SpectateID, Server()->ClientName(SpectateID));
				str_format(aCmd, sizeof(aCmd), "set_team %d -1 %d", SpectateID, g_Config.m_SvVoteSpectateRejoindelay);
				char aBuf[128];
				str_format(aBuf, sizeof(aBuf),
					"'%d:%s' voted %s '%d:%s' reason='%s' cmd='%s' force=%d",
					ClientID, Server()->ClientName(ClientID), pMsg->m_Type,
					SpectateID, Server()->ClientName(SpectateID), pReason, aCmd, pMsg->m_Force
				);
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
				if(pMsg->m_Force)
				{
					Server()->SetRconCID(ClientID);
					Console()->ExecuteLine(aCmd);
					Server()->SetRconCID(IServer::RCON_CID_SERV);
					ForceVote(VOTE_START_SPEC, aDesc, pReason);
					return;
				}
				m_VoteType = VOTE_START_SPEC;
				m_VoteClientID = SpectateID;
			}

			if(m_VoteType != VOTE_UNKNOWN)
			{
				m_VoteCreator = ClientID;
				StartVote(aDesc, aCmd, pReason);
				pPlayer->m_Vote = 1;
				pPlayer->m_VotePos = m_VotePos = 1;
				pPlayer->m_LastVoteCall = Now;
			}
		}
		else if(MsgID == NETMSGTYPE_CL_VOTE)
		{
			if(!m_VoteCloseTime)
				return;

			if(pPlayer->m_Vote == 0)
			{
				CNetMsg_Cl_Vote *pMsg = (CNetMsg_Cl_Vote *)pRawMsg;
				if(!pMsg->m_Vote)
					return;

				pPlayer->m_Vote = pMsg->m_Vote;
				pPlayer->m_VotePos = ++m_VotePos;
				m_VoteUpdate = true;
			}
			else if(m_VoteCreator == pPlayer->GetCID())
			{
				CNetMsg_Cl_Vote *pMsg = (CNetMsg_Cl_Vote *)pRawMsg;
				if(pMsg->m_Vote != -1 || m_VoteCancelTime<time_get())
					return;

				m_VoteCloseTime = -1;
			}
		}
		else if(MsgID == NETMSGTYPE_CL_SETTEAM && m_pController->IsTeamChangeAllowed())
		{
			CNetMsg_Cl_SetTeam *pMsg = (CNetMsg_Cl_SetTeam *)pRawMsg;

			if ((pPlayer->GetCharacter() && pPlayer->GetCharacter()->IsFrozen()) || (pPlayer->GetTeam() == pMsg->m_Team || (g_Config.m_SvSpamprotection && pPlayer->m_LastSetTeam && pPlayer->m_LastSetTeam + Server()->TickSpeed() * 3 > Server()->Tick())))
				return;

			pPlayer->m_LastSetTeam = Server()->Tick();

			// Switch team on given client and kill/respawn him
			if(m_pController->CanJoinTeam(pMsg->m_Team, ClientID) && m_pController->CanChangeTeam(pPlayer, pMsg->m_Team))
			{
				if(pPlayer->GetTeam() == TEAM_SPECTATORS || pMsg->m_Team == TEAM_SPECTATORS)
					m_VoteUpdate = true;
				pPlayer->m_TeamChangeTick = Server()->Tick()+Server()->TickSpeed()*3;
				m_pController->DoTeamChange(pPlayer, pMsg->m_Team);
			}
		}
		else if (MsgID == NETMSGTYPE_CL_SETSPECTATORMODE && !m_World.m_Paused)
		{
			CNetMsg_Cl_SetSpectatorMode *pMsg = (CNetMsg_Cl_SetSpectatorMode *)pRawMsg;

			if(g_Config.m_SvSpamprotection && pPlayer->m_LastSetSpectatorMode && pPlayer->m_LastSetSpectatorMode+Server()->TickSpeed() > Server()->Tick())
				return;

			pPlayer->m_LastSetSpectatorMode = Server()->Tick();
			if(!pPlayer->SetSpectatorID(pMsg->m_SpecMode, pMsg->m_SpectatorID))
				SendGameMsg(GAMEMSG_SPEC_INVALIDID, ClientID);
		}
		else if (MsgID == NETMSGTYPE_CL_EMOTICON && !m_World.m_Paused)
		{
			CNetMsg_Cl_Emoticon *pMsg = (CNetMsg_Cl_Emoticon *)pRawMsg;

			if(g_Config.m_SvSpamprotection && pPlayer->m_LastEmote && pPlayer->m_LastEmote+Server()->TickSpeed()*g_Config.m_SvEmoticonDelay > Server()->Tick())
				return;

			pPlayer->m_LastEmote = Server()->Tick();

			++pPlayer->m_Stats.m_NumEmotes;

			SendEmoticon(ClientID, pMsg->m_Emoticon);

			int EmoteIcon = EMOTE_NORMAL;
			switch (pMsg->m_Emoticon)
			{
			case EMOTICON_OOP: EmoteIcon = EMOTE_PAIN; break;
			case EMOTICON_EXCLAMATION: EmoteIcon = EMOTE_SURPRISE; break;
			case EMOTICON_HEARTS: EmoteIcon = EMOTE_HAPPY; break;
			case EMOTICON_DROP: EmoteIcon = EMOTE_BLINK; break;
			case EMOTICON_DOTDOT: EmoteIcon = EMOTE_BLINK; break;
			case EMOTICON_MUSIC: EmoteIcon = EMOTE_HAPPY; break;
			case EMOTICON_SORRY: EmoteIcon = EMOTE_PAIN; break;
			case EMOTICON_GHOST: EmoteIcon = EMOTE_SURPRISE; break;
			case EMOTICON_SUSHI: EmoteIcon = EMOTE_PAIN; break;
			case EMOTICON_SPLATTEE: EmoteIcon = EMOTE_ANGRY;  break;
			case EMOTICON_DEVILTEE: EmoteIcon = EMOTE_ANGRY; break;
			case EMOTICON_ZOMG: EmoteIcon = EMOTE_ANGRY; break;
			case EMOTICON_ZZZ: EmoteIcon = EMOTE_BLINK; break;
			case EMOTICON_WTF: EmoteIcon = EMOTE_SURPRISE; break;
			case EMOTICON_EYES: EmoteIcon = EMOTE_HAPPY; break;
			case EMOTICON_QUESTION: EmoteIcon = EMOTE_SURPRISE; break;
			}
			if (pPlayer->GetCharacter())
				pPlayer->GetCharacter()->SetEmote(EmoteIcon, Server()->Tick() + Server()->TickSpeed()*2.0f);

		}
		else if (MsgID == NETMSGTYPE_CL_KILL && !m_World.m_Paused)
		{
			if((pPlayer->GetCharacter() && pPlayer->GetCharacter()->IsFrozen()) || (pPlayer->m_LastKill && pPlayer->m_LastKill+Server()->TickSpeed()*g_Config.m_SvKillDelay > Server()->Tick()) || g_Config.m_SvKillDelay == -1)
				return;

			pPlayer->m_LastKill = Server()->Tick();
			pPlayer->KillCharacter(WEAPON_SELF);
		}
		else if (MsgID == NETMSGTYPE_CL_READYCHANGE)
		{
			if(pPlayer->m_LastReadyChange && pPlayer->m_LastReadyChange+Server()->TickSpeed()*1 > Server()->Tick())
				return;

			pPlayer->m_LastReadyChange = Server()->Tick();
			m_pController->OnPlayerReadyChange(pPlayer);
		}
		else if(MsgID == NETMSGTYPE_CL_SKINCHANGE)
		{
			if(pPlayer->m_LastChangeInfo && pPlayer->m_LastChangeInfo+Server()->TickSpeed()*5 > Server()->Tick())
				return;

			pPlayer->m_LastChangeInfo = Server()->Tick();
			CNetMsg_Cl_SkinChange *pMsg = (CNetMsg_Cl_SkinChange *)pRawMsg;

			for(int p = 0; p < NUM_SKINPARTS; p++)
			{
				str_copy(pPlayer->m_TeeInfos.m_aaSkinPartNames[p], pMsg->m_apSkinPartNames[p], 24);
				pPlayer->m_TeeInfos.m_aUseCustomColors[p] = pMsg->m_aUseCustomColors[p];
				pPlayer->m_TeeInfos.m_aSkinPartColors[p] = pMsg->m_aSkinPartColors[p];
			}

			// update all clients
			for(int i = 0; i < MAX_CLIENTS; ++i)
			{
				if(!m_apPlayers[i] || (!Server()->ClientIngame(i) && !m_apPlayers[i]->IsDummy()) || Server()->GetClientVersion(i) < MIN_SKINCHANGE_CLIENTVERSION)
					continue;

				SendSkinChange(pPlayer->GetCID(), i);
			}

			m_pController->OnPlayerInfoChange(pPlayer);
		}
		else if (MsgID == NETMSGTYPE_CL_COMMAND)
		{
			CNetMsg_Cl_Command *pMsg = (CNetMsg_Cl_Command*)pRawMsg;
			if(!OnPlayerCommand(pPlayer->GetCID(), pMsg->m_Name, pMsg->m_Arguments))
				m_pController->OnPlayerCommand(pPlayer, pMsg->m_Name, pMsg->m_Arguments);
		}
	}
	else
	{
		if (MsgID == NETMSGTYPE_CL_STARTINFO)
		{
			if(pPlayer->m_IsReadyToEnter)
				return;

			CNetMsg_Cl_StartInfo *pMsg = (CNetMsg_Cl_StartInfo *)pRawMsg;
			pPlayer->m_LastChangeInfo = Server()->Tick();

			// set start infos
			Server()->SetClientName(ClientID, pMsg->m_pName);
			Server()->SetClientClan(ClientID, pMsg->m_pClan);
			Server()->SetClientCountry(ClientID, pMsg->m_Country);

			for(int p = 0; p < NUM_SKINPARTS; p++)
			{
				str_copy(pPlayer->m_TeeInfos.m_aaSkinPartNames[p], pMsg->m_apSkinPartNames[p], 24);
				pPlayer->m_TeeInfos.m_aUseCustomColors[p] = pMsg->m_aUseCustomColors[p];
				pPlayer->m_TeeInfos.m_aSkinPartColors[p] = pMsg->m_aSkinPartColors[p];
			}

			m_pController->OnPlayerInfoChange(pPlayer);

			// send vote options
			CNetMsg_Sv_VoteClearOptions ClearMsg;
			Server()->SendPackMsg(&ClearMsg, MSGFLAG_VITAL, ClientID);

			CVoteOptionServer *pCurrent = m_pVoteOptionFirst;
			while(pCurrent)
			{
				// count options for actual packet
				int NumOptions = 0;
				for(CVoteOptionServer *p = pCurrent; p && NumOptions < MAX_VOTE_OPTION_ADD; p = p->m_pNext, ++NumOptions);

				// pack and send vote list packet
				CMsgPacker Msg(NETMSGTYPE_SV_VOTEOPTIONLISTADD);
				Msg.AddInt(NumOptions);
				while(pCurrent && NumOptions--)
				{
					Msg.AddString(pCurrent->m_aDescription, VOTE_DESC_LENGTH);
					pCurrent = pCurrent->m_pNext;
				}
				Server()->SendMsg(&Msg, MSGFLAG_VITAL, ClientID);
			}

			// send tuning parameters to client
			SendTuningParams(ClientID);

			// client is ready to enter
			pPlayer->m_IsReadyToEnter = true;
			CNetMsg_Sv_ReadyToEnter m;
			Server()->SendPackMsg(&m, MSGFLAG_VITAL|MSGFLAG_FLUSH, ClientID);
		}
	}
}

sServerCommand* CGameContext::FindCommand(const char* pCmd)
{
	if(!pCmd)
		return NULL;
	sServerCommand* pFindCmd = m_FirstServerCommand;
	while(pFindCmd)
	{
		if(str_comp_nocase_whitespace(pFindCmd->m_Cmd, pCmd) == 0)
			return pFindCmd;
		pFindCmd = pFindCmd->m_NextCommand;
	}
	return NULL;
}

void CGameContext::AddServerCommandSorted(sServerCommand* pCmd)
{
	if(!m_FirstServerCommand)
	{
		m_FirstServerCommand = pCmd;
	}
	else
	{
		sServerCommand* pFindCmd = m_FirstServerCommand;
		sServerCommand* pPrevFindCmd = 0;
		while(pFindCmd)
		{
			if(str_comp_nocase(pCmd->m_Cmd, pFindCmd->m_Cmd) < 0)
			{
				if(pPrevFindCmd)
				{
					pPrevFindCmd->m_NextCommand = pCmd;
				}
				else
				{
					m_FirstServerCommand = pCmd;
				}
				pCmd->m_NextCommand = pFindCmd;
				return;
			}
			pPrevFindCmd = pFindCmd;
			pFindCmd = pFindCmd->m_NextCommand;
		}
		pPrevFindCmd->m_NextCommand = pCmd;
	}
}

void CGameContext::SendPlayerCommands(int ClientID)
{
	sServerCommand* pItCmd = m_FirstServerCommand;
	while(pItCmd)
	{
		char aName[128];
		char aHelpText[128];
		char aArgsFormat[128];

		str_format(aName, sizeof(aName), "%s", pItCmd->m_Cmd);
		str_format(aHelpText, sizeof(aHelpText), "%s", pItCmd->m_Desc);
		str_format(aArgsFormat, sizeof(aArgsFormat), "%s", (pItCmd->m_ArgFormat) ? pItCmd->m_ArgFormat : "");

		CNetMsg_Sv_CommandInfo Msg;
		Msg.m_pName = aName;
		Msg.m_HelpText = aHelpText;
		Msg.m_ArgsFormat = aArgsFormat;

		Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientID);

		pItCmd = pItCmd->m_NextCommand;
	}
}

bool CGameContext::OnPlayerCommand(int ClientID, const char *pCommandName, const char *pCommandArgs)
{
	size_t LineLen = str_length(pCommandArgs + 1);

	if(LineLen == 0 || LineLen > 128)
		return false;
	else
	{
		char aLine[128 + 1];
		str_format(aLine, sizeof(aLine), "%s", pCommandArgs + 1);
		return ExecuteServerCommand(ClientID, aLine);
	}
	
}

void CGameContext::AddServerCommand(const char* pCmd, const char* pDesc, const char* pArgFormat, ServerCommandExecuteFunc pFunc)
{
	if(!pCmd)
		return;
	sServerCommand* pFindCmd = FindCommand(pCmd);
	if(!pFindCmd)
	{
		pFindCmd = new sServerCommand(pCmd, pDesc, pArgFormat, pFunc);	
		AddServerCommandSorted(pFindCmd);
	}
	else
	{
		pFindCmd->m_Func = pFunc;
		pFindCmd->m_Desc = pDesc;
		pFindCmd->m_ArgFormat = pArgFormat;
	}
}

bool CGameContext::ExecuteServerCommand(int pClientID, const char* pLine)
{
	if(!pLine || !*pLine)
		return false;
	const char* pCmdEnd = pLine;
	while(*pCmdEnd && !is_whitespace(*pCmdEnd))
		++pCmdEnd;
	
	const char* pArgs = (*pCmdEnd) ? pCmdEnd + 1 : 0;
	
	sServerCommand* pCmd = FindCommand(pLine);
	if(pCmd)
	{
		pCmd->ExecuteCommand(this, pClientID, pArgs);
		return true;
	}
	else
	{
		SendChatTarget(pClientID, "Server command not found");
		return false;
	}
}

void CGameContext::CmdStats(CGameContext* pContext, int pClientID, const char** pArgs, int ArgNum)
{
	CPlayer* p = pContext->m_apPlayers[pClientID];
	
	char aBuff[900];

	str_format(aBuff, 900, "╔════════ Statistics ════════\n"
		"║\n"
		"║Kills(weapon): %d\n"
		"║Hits(By opponent's weapon): %d\n"
		"║\n"
		"║Kills/Deaths: %4.2f\n"
		"║Shots | Kills/Shots: %d | %3.1f%%\n"
		"║\n"
		"╠═════════ Spikes ═════════\n"
		"║\n"
		"║Kills(normal spikes): %d\n"
		"║Kills(team spikes): %d\n"
		"║Kills(golden spikes): %d\n"
		"║Kills(false spikes): %d\n"
		"║Spike deaths(while frozen): %d\n"
		"║\n"
		"╠══════════ Misc ═════════\n"
		"║\n"
		"║Mates hammer-/unfrozen: %d/%d\n"
		"║\n"
		"╚════════════════════════", p->m_Stats.m_Kills, p->m_Stats.m_Hits, (p->m_Stats.m_Hits != 0) ? (float)((float)p->m_Stats.m_Kills / (float)p->m_Stats.m_Hits) : (float)p->m_Stats.m_Kills, p->m_Stats.m_Shots, ((float)p->m_Stats.m_Kills / (float)(p->m_Stats.m_Shots == 0 ? 1: p->m_Stats.m_Shots)) * 100.f, p->m_Stats.m_GrabsNormal, p->m_Stats.m_GrabsTeam, p->m_Stats.m_GrabsGold, p->m_Stats.m_GrabsFalse, p->m_Stats.m_Deaths, p->m_Stats.m_UnfreezingHammerHits, p->m_Stats.m_Unfreezes);

	CNetMsg_Sv_Motd Msg;
	Msg.m_pMessage = aBuff;
	pContext->Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, pClientID);
}

void CGameContext::CmdMe(CGameContext* pContext, int pClientID, const char** pArgs, int ArgNum)
{
	if (ArgNum != 0)
	{

		char aBuff[200];

		str_format(aBuff, 200, "# {%s}", pArgs[0]);
		pContext->SendChat(-1, CHAT_ALL, -1, aBuff);

	}
}

void CGameContext::CmdHelp(CGameContext* pContext, int pClientID, const char** pArgs, int ArgNum)
{
	if (ArgNum != 0)
	{
		sServerCommand* pCmd = pContext->FindCommand(pArgs[0]);
		if(pCmd)
		{
			char aBuff[200];
			str_format(aBuff, 200, "[/%s] %s", pCmd->m_Cmd, pCmd->m_Desc);
			pContext->SendChatTarget(pClientID, aBuff);
		}
	}
	else
	{
		pContext->SendChatTarget(pClientID, "command list: type /help <command> for more information");
		
		sServerCommand* pCmd = pContext->m_FirstServerCommand;
		char aBuff[200];
		while(pCmd)
		{
			str_format(aBuff, 200, "/%s %s", pCmd->m_Cmd, (pCmd->m_ArgFormat) ? pCmd->m_ArgFormat : "");
			pContext->SendChatTarget(pClientID, aBuff);
			pCmd = pCmd->m_NextCommand;			
		}
	}
}

void CGameContext::CmdEmote(CGameContext* pContext, int pClientID, const char** pArgs, int ArgNum)
{
	CPlayer* pPlayer = pContext->m_apPlayers[pClientID];
	
	if (ArgNum > 0)
	{
		if (!str_comp_nocase_whitespace(pArgs[0], "angry"))
				pPlayer->m_Emotion = EMOTE_ANGRY;
			else if (!str_comp_nocase_whitespace(pArgs[0], "blink"))
				pPlayer->m_Emotion = EMOTE_BLINK;
			else if (!str_comp_nocase_whitespace(pArgs[0], "close"))
				pPlayer->m_Emotion = EMOTE_BLINK;
			else if (!str_comp_nocase_whitespace(pArgs[0], "happy"))
				pPlayer->m_Emotion = EMOTE_HAPPY;
			else if (!str_comp_nocase_whitespace(pArgs[0], "pain"))
				pPlayer->m_Emotion = EMOTE_PAIN;
			else if (!str_comp_nocase_whitespace(pArgs[0], "surprise"))
				pPlayer->m_Emotion = EMOTE_SURPRISE;
			else if (!str_comp_nocase_whitespace(pArgs[0], "normal"))
				pPlayer->m_Emotion = EMOTE_NORMAL;
			else
				pContext->SendChatTarget(pClientID, "Unknown emote... Say /emote");
			
			int Duration = pContext->Server()->TickSpeed();
			if(ArgNum > 1)
				Duration = str_toint(pArgs[1]);
			
			pPlayer->m_EmotionDuration = Duration * pContext->Server()->TickSpeed();
	} else
	{
		//ddrace like
		pContext->SendChatTarget(pClientID, "Emote commands are: /emote surprise /emote blink /emote close /emote angry /emote happy /emote pain");
		pContext->SendChatTarget(pClientID, "Example: /emote surprise 10 for 10 seconds or /emote surprise (default 1 second)");
	}
}

void CGameContext::ConTuneParam(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	const char *pParamName = pResult->GetString(0);
	float NewValue = pResult->GetFloat(1);

	if(pSelf->Tuning()->Set(pParamName, NewValue))
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "%s changed to %.2f", pParamName, NewValue);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
		pSelf->SendTuningParams(-1);
	}
	else
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "No such tuning parameter");
}

void CGameContext::ConTuneReset(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	CTuningParams TuningParams;
	*pSelf->Tuning() = TuningParams;
	pSelf->SendTuningParams(-1);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", "Tuning reset");
}

void CGameContext::ConTuneDump(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	char aBuf[256];
	for(int i = 0; i < pSelf->Tuning()->Num(); i++)
	{
		float v;
		pSelf->Tuning()->Get(i, &v);
		str_format(aBuf, sizeof(aBuf), "%s %.2f", pSelf->Tuning()->m_apNames[i], v);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tuning", aBuf);
	}
}

void CGameContext::ConPause(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;

	if(pResult->NumArguments())
		pSelf->m_pController->DoPause(clamp(pResult->GetInteger(0), -1, 1000));
	else
		pSelf->m_pController->DoPause(pSelf->m_pController->IsGamePaused() ? 0 : IGameController::TIMER_INFINITE);
}

void CGameContext::ConChangeMap(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->m_pController->ChangeMap(pResult->NumArguments() ? pResult->GetString(0) : "");
}

void CGameContext::ConRestart(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(pResult->NumArguments())
		pSelf->m_pController->DoWarmup(clamp(pResult->GetInteger(0), -1, 1000));
	else
		pSelf->m_pController->DoWarmup(0);
}

void CGameContext::ConSay(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->SendChat(-1, CHAT_ALL, -1, pResult->GetString(0));
}

void CGameContext::ConBroadcast(IConsole::IResult* pResult, void* pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->SendBroadcast(pResult->GetString(0), -1);
}

void CGameContext::ConSetTeam(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	int ClientID = clamp(pResult->GetInteger(0), 0, (int)MAX_CLIENTS-1);
	int Team = clamp(pResult->GetInteger(1), -1, 1);
	int Delay = pResult->NumArguments()>2 ? pResult->GetInteger(2) : 0;
	if(!pSelf->m_apPlayers[ClientID] || !pSelf->m_pController->CanJoinTeam(Team, ClientID))
		return;

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "moved client %d to team %d", ClientID, Team);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	pSelf->m_apPlayers[ClientID]->m_TeamChangeTick = pSelf->Server()->Tick()+pSelf->Server()->TickSpeed()*Delay*60;
	pSelf->m_pController->DoTeamChange(pSelf->m_apPlayers[ClientID], Team);
}

void CGameContext::ConSetTeamAll(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	int Team = clamp(pResult->GetInteger(0), -1, 1);

	pSelf->SendGameMsg(GAMEMSG_TEAM_ALL, Team, -1);

	for(int i = 0; i < MAX_CLIENTS; ++i)
		if(pSelf->m_apPlayers[i] && pSelf->m_pController->CanJoinTeam(Team, i))
			pSelf->m_pController->DoTeamChange(pSelf->m_apPlayers[i], Team, false);
}

void CGameContext::ConSwapTeams(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->SwapTeams();
}

void CGameContext::ConShuffleTeams(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!pSelf->m_pController->IsTeamplay())
		return;

	int rnd = 0;
	int PlayerTeam = 0;
	int aPlayer[MAX_CLIENTS];

	for(int i = 0; i < MAX_CLIENTS; i++)
		if(pSelf->m_apPlayers[i] && pSelf->m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS)
			aPlayer[PlayerTeam++]=i;

	pSelf->SendGameMsg(GAMEMSG_TEAM_SHUFFLE, -1);

	//creating random permutation
	for(int i = PlayerTeam; i > 1; i--)
	{
		rnd = random_int() % i;
		int tmp = aPlayer[rnd];
		aPlayer[rnd] = aPlayer[i-1];
		aPlayer[i-1] = tmp;
	}
	//uneven Number of Players?
	rnd = PlayerTeam % 2 ? random_int() % 2 : 0;

	for(int i = 0; i < PlayerTeam; i++)
		pSelf->m_pController->DoTeamChange(pSelf->m_apPlayers[aPlayer[i]], i < (PlayerTeam+rnd)/2 ? TEAM_RED : TEAM_BLUE, false);
}

void CGameContext::ConLockTeams(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->m_LockTeams ^= 1;
	pSelf->SendSettings(-1);
}

void CGameContext::ConForceTeamBalance(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->m_pController->ForceTeamBalance();
}

void CGameContext::ConAddVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	const char *pDescription = pResult->GetString(0);
	const char *pCommand = pResult->GetString(1);

	if(pSelf->m_NumVoteOptions == MAX_VOTE_OPTIONS)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "maximum number of vote options reached");
		return;
	}

	// check for valid option
	if(!pSelf->Console()->LineIsValid(pCommand) || str_length(pCommand) >= VOTE_CMD_LENGTH)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "skipped invalid command '%s'", pCommand);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}
	while(*pDescription == ' ')
		pDescription++;
	if(str_length(pDescription) >= VOTE_DESC_LENGTH || *pDescription == 0)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "skipped invalid option '%s'", pDescription);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}

	// check for duplicate entry
	CVoteOptionServer *pOption = pSelf->m_pVoteOptionFirst;
	while(pOption)
	{
		if(str_comp_nocase(pDescription, pOption->m_aDescription) == 0)
		{
			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "option '%s' already exists", pDescription);
			pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
			return;
		}
		pOption = pOption->m_pNext;
	}

	// add the option
	++pSelf->m_NumVoteOptions;
	int Len = str_length(pCommand);

	pOption = (CVoteOptionServer *)pSelf->m_pVoteOptionHeap->Allocate(sizeof(CVoteOptionServer) + Len);
	pOption->m_pNext = 0;
	pOption->m_pPrev = pSelf->m_pVoteOptionLast;
	if(pOption->m_pPrev)
		pOption->m_pPrev->m_pNext = pOption;
	pSelf->m_pVoteOptionLast = pOption;
	if(!pSelf->m_pVoteOptionFirst)
		pSelf->m_pVoteOptionFirst = pOption;

	str_copy(pOption->m_aDescription, pDescription, sizeof(pOption->m_aDescription));
	mem_copy(pOption->m_aCommand, pCommand, Len+1);
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "added option '%s' '%s'", pOption->m_aDescription, pOption->m_aCommand);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	// inform clients about added option
	CNetMsg_Sv_VoteOptionAdd OptionMsg;
	OptionMsg.m_pDescription = pOption->m_aDescription;
	pSelf->Server()->SendPackMsg(&OptionMsg, MSGFLAG_VITAL, -1);
}

void CGameContext::ConRemoveVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	const char *pDescription = pResult->GetString(0);

	// check for valid option
	CVoteOptionServer *pOption = pSelf->m_pVoteOptionFirst;
	while(pOption)
	{
		if(str_comp_nocase(pDescription, pOption->m_aDescription) == 0)
			break;
		pOption = pOption->m_pNext;
	}
	if(!pOption)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "option '%s' does not exist", pDescription);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
		return;
	}

	// inform clients about removed option
	CNetMsg_Sv_VoteOptionRemove OptionMsg;
	OptionMsg.m_pDescription = pOption->m_aDescription;
	pSelf->Server()->SendPackMsg(&OptionMsg, MSGFLAG_VITAL, -1);

	// TODO: improve this
	// remove the option
	--pSelf->m_NumVoteOptions;
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "removed option '%s' '%s'", pOption->m_aDescription, pOption->m_aCommand);
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	CHeap *pVoteOptionHeap = new CHeap();
	CVoteOptionServer *pVoteOptionFirst = 0;
	CVoteOptionServer *pVoteOptionLast = 0;
	int NumVoteOptions = pSelf->m_NumVoteOptions;
	for(CVoteOptionServer *pSrc = pSelf->m_pVoteOptionFirst; pSrc; pSrc = pSrc->m_pNext)
	{
		if(pSrc == pOption)
			continue;

		// copy option
		int Len = str_length(pSrc->m_aCommand);
		CVoteOptionServer *pDst = (CVoteOptionServer *)pVoteOptionHeap->Allocate(sizeof(CVoteOptionServer) + Len);
		pDst->m_pNext = 0;
		pDst->m_pPrev = pVoteOptionLast;
		if(pDst->m_pPrev)
			pDst->m_pPrev->m_pNext = pDst;
		pVoteOptionLast = pDst;
		if(!pVoteOptionFirst)
			pVoteOptionFirst = pDst;

		str_copy(pDst->m_aDescription, pSrc->m_aDescription, sizeof(pDst->m_aDescription));
		mem_copy(pDst->m_aCommand, pSrc->m_aCommand, Len+1);
	}

	// clean up
	delete pSelf->m_pVoteOptionHeap;
	pSelf->m_pVoteOptionHeap = pVoteOptionHeap;
	pSelf->m_pVoteOptionFirst = pVoteOptionFirst;
	pSelf->m_pVoteOptionLast = pVoteOptionLast;
	pSelf->m_NumVoteOptions = NumVoteOptions;
}

void CGameContext::ConClearVotes(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;

	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "cleared votes");
	CNetMsg_Sv_VoteClearOptions VoteClearOptionsMsg;
	pSelf->Server()->SendPackMsg(&VoteClearOptionsMsg, MSGFLAG_VITAL, -1);
	pSelf->m_pVoteOptionHeap->Reset();
	pSelf->m_pVoteOptionFirst = 0;
	pSelf->m_pVoteOptionLast = 0;
	pSelf->m_NumVoteOptions = 0;
}

void CGameContext::ConVote(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;

	// check if there is a vote running
	if(!pSelf->m_VoteCloseTime)
		return;

	if(str_comp_nocase(pResult->GetString(0), "yes") == 0)
		pSelf->m_VoteEnforce = CGameContext::VOTE_ENFORCE_YES;
	else if(str_comp_nocase(pResult->GetString(0), "no") == 0)
		pSelf->m_VoteEnforce = CGameContext::VOTE_ENFORCE_NO;
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "forcing vote %s", pResult->GetString(0));
	pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
}

void CGameContext::ConchainSpecialMotdupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CGameContext *pSelf = (CGameContext *)pUserData;
		pSelf->SendMotd(-1);
	}
}

void CGameContext::ConchainSettingUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CGameContext *pSelf = (CGameContext *)pUserData;
		if(pSelf->Server()->MaxClients() < g_Config.m_SvPlayerSlots)
			g_Config.m_SvPlayerSlots = pSelf->Server()->MaxClients();
		pSelf->SendSettings(-1);
	}
}

void CGameContext::ConchainGameinfoUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CGameContext *pSelf = (CGameContext *)pUserData;
		if(pSelf->m_pController)
			pSelf->m_pController->CheckGameInfo();
	}
}

void CGameContext::OnConsoleInit()
{
	m_pServer = Kernel()->RequestInterface<IServer>();
	m_pConsole = Kernel()->RequestInterface<IConsole>();

	Console()->Register("tune", "si", CFGFLAG_SERVER, ConTuneParam, this, "Tune variable to value");
	Console()->Register("tune_reset", "", CFGFLAG_SERVER, ConTuneReset, this, "Reset tuning");
	Console()->Register("tune_dump", "", CFGFLAG_SERVER, ConTuneDump, this, "Dump tuning");

	Console()->Register("pause", "?i", CFGFLAG_SERVER|CFGFLAG_STORE, ConPause, this, "Pause/unpause game");
	Console()->Register("change_map", "?r", CFGFLAG_SERVER|CFGFLAG_STORE, ConChangeMap, this, "Change map");
	Console()->Register("restart", "?i", CFGFLAG_SERVER|CFGFLAG_STORE, ConRestart, this, "Restart in x seconds (0 = abort)");
	Console()->Register("say", "r", CFGFLAG_SERVER, ConSay, this, "Say in chat");
	Console()->Register("broadcast", "r", CFGFLAG_SERVER, ConBroadcast, this, "Broadcast message");
	Console()->Register("set_team", "ii?i", CFGFLAG_SERVER, ConSetTeam, this, "Set team of player to team");
	Console()->Register("set_team_all", "i", CFGFLAG_SERVER, ConSetTeamAll, this, "Set team of all players to team");
	Console()->Register("swap_teams", "", CFGFLAG_SERVER, ConSwapTeams, this, "Swap the current teams");
	Console()->Register("shuffle_teams", "", CFGFLAG_SERVER, ConShuffleTeams, this, "Shuffle the current teams");
	Console()->Register("lock_teams", "", CFGFLAG_SERVER, ConLockTeams, this, "Lock/unlock teams");
	Console()->Register("force_teambalance", "", CFGFLAG_SERVER, ConForceTeamBalance, this, "Force team balance");

	Console()->Register("add_vote", "sr", CFGFLAG_SERVER, ConAddVote, this, "Add a voting option");
	Console()->Register("remove_vote", "s", CFGFLAG_SERVER, ConRemoveVote, this, "remove a voting option");
	Console()->Register("clear_votes", "", CFGFLAG_SERVER, ConClearVotes, this, "Clears the voting options");
	Console()->Register("vote", "r", CFGFLAG_SERVER, ConVote, this, "Force a vote to yes/no");
}

void CGameContext::OnInit()
{
	// init everything
	m_pServer = Kernel()->RequestInterface<IServer>();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_World.SetGameServer(this);
	m_Events.SetGameServer(this);

	AddServerCommand("stats", "show the stats of the current game", 0, CmdStats);
	AddServerCommand("s", "show the stats of the current game", 0, CmdStats);
	AddServerCommand("help", "show the cmd list or get more information to any command", "<command>", CmdHelp);
	AddServerCommand("cmdlist", "show the cmd list", 0, CmdHelp);
	AddServerCommand("me", "sending message to chat", 0, CmdMe);
	if(g_Config.m_SvEmoteWheel || g_Config.m_SvEmotionalTees)
		AddServerCommand("emote", "enable custom emotes", "<emote type> <time in seconds>", CmdEmote);

	// HACK: only set static size for items, which were available in the first 0.7 release
	// so new items don't break the snapshot delta
	static const int OLD_NUM_NETOBJTYPES = 23;
	for(int i = 0; i < OLD_NUM_NETOBJTYPES; i++)
		Server()->SnapSetStaticsize(i, m_NetObjHandler.GetObjSize(i));

	m_Layers.Init(Kernel());
	m_Collision.Init(&m_Layers);

	// select gametype
	if (str_comp(g_Config.m_SvGametype, "fng2") == 0)
		m_pController = new CGameControllerFNG2(this);
	else if (str_comp(g_Config.m_SvGametype, "fng2solo") == 0)
		m_pController = new CGameControllerFNG2Solo(this);
	else if (str_comp(g_Config.m_SvGametype, "fng2boom") == 0)
		m_pController = new CGameControllerFNG2Boom(this);
	else if (str_comp(g_Config.m_SvGametype, "fng2boomsolo") == 0)
		m_pController = new CGameControllerFNG2BoomSolo(this);
	//else if (str_comp(g_Config.m_SvGametype, "fng24teams") == 0)
	//	m_pController = new CGameControllerFNG24Teams(this);
	else 
#define CONTEXT_INIT_WITHOUT_CONFIG
#include "gamecontext_additional_gametypes.h"
#undef CONTEXT_INIT_WITHOUT_CONFIG
		m_pController = new CGameControllerFNG2(this);
	
	if(g_Config.m_SvEmoteWheel) 
		m_pController->m_pGameType = "fng2+";

	// create all entities from the game layer
	CMapItemLayerTilemap *pTileMap = m_Layers.GameLayer();
	CTile *pTiles = (CTile *)Kernel()->RequestInterface<IMap>()->GetData(pTileMap->m_Data);
	for(int y = 0; y < pTileMap->m_Height; y++)
	{
		for(int x = 0; x < pTileMap->m_Width; x++)
		{
			int Index = pTiles[y*pTileMap->m_Width+x].m_Index;

			if(Index >= 128)
			{
				vec2 Pos(x*32.0f+16.0f, y*32.0f+16.0f);
				m_pController->OnEntity(Index-ENTITY_OFFSET, Pos);
			}
		}
	}

	Console()->Chain("sv_motd", ConchainSpecialMotdupdate, this);

	Console()->Chain("sv_vote_kick", ConchainSettingUpdate, this);
	Console()->Chain("sv_vote_kick_min", ConchainSettingUpdate, this);
	Console()->Chain("sv_vote_spectate", ConchainSettingUpdate, this);
	Console()->Chain("sv_teambalance_time", ConchainSettingUpdate, this);
	Console()->Chain("sv_player_slots", ConchainSettingUpdate, this);

	Console()->Chain("sv_scorelimit", ConchainGameinfoUpdate, this);
	Console()->Chain("sv_timelimit", ConchainGameinfoUpdate, this);
	Console()->Chain("sv_matches_per_map", ConchainGameinfoUpdate, this);

	// clamp sv_player_slots to 0..MaxClients
	if(Server()->MaxClients() < g_Config.m_SvPlayerSlots)
		g_Config.m_SvPlayerSlots = Server()->MaxClients();

#ifdef CONF_DEBUG
	// clamp dbg_dummies to 0..MaxClients-1
	if(Server()->MaxClients() <= g_Config.m_DbgDummies)
		g_Config.m_DbgDummies = Server()->MaxClients();
	if(g_Config.m_DbgDummies)
	{
		for(int i = 0; i < g_Config.m_DbgDummies ; i++)
			OnClientConnected(Server()->MaxClients() -i-1, true, false);
	}
#endif
}

void CGameContext::OnShutdown()
{
	delete m_pController;
	m_pController = 0;
	Clear();
}

void CGameContext::OnSnap(int ClientID)
{
	// add tuning to demo
	CTuningParams StandardTuning;
	if(ClientID == -1 && Server()->DemoRecorder_IsRecording() && mem_comp(&StandardTuning, &m_Tuning, sizeof(CTuningParams)) != 0)
	{
		CNetObj_De_TuneParams *pTuneParams = static_cast<CNetObj_De_TuneParams *>(Server()->SnapNewItem(NETOBJTYPE_DE_TUNEPARAMS, 0, sizeof(CNetObj_De_TuneParams)));
		if(!pTuneParams)
			return;

		mem_copy(pTuneParams->m_aTuneParams, &m_Tuning, sizeof(pTuneParams->m_aTuneParams));
	}

	m_World.Snap(ClientID);
	m_pController->Snap(ClientID);
	m_Events.Snap(ClientID);

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_apPlayers[i])
			m_apPlayers[i]->Snap(ClientID);
	}
}
void CGameContext::OnPreSnap() {}
void CGameContext::OnPostSnap()
{
	m_World.PostSnap();
	m_Events.Clear();
}

bool CGameContext::IsClientReady(int ClientID) const
{
	return m_apPlayers[ClientID] && m_apPlayers[ClientID]->m_IsReadyToEnter;
}

bool CGameContext::IsClientPlayer(int ClientID) const
{
	return m_apPlayers[ClientID] && m_apPlayers[ClientID]->GetTeam() != TEAM_SPECTATORS;
}

bool CGameContext::IsClientSpectator(int ClientID) const
{
	return m_apPlayers[ClientID] && m_apPlayers[ClientID]->GetTeam() == TEAM_SPECTATORS;
}

const char *CGameContext::GameType() const { return m_pController && m_pController->GetGameType() ? m_pController->GetGameType() : ""; }
const char *CGameContext::Version() const { return g_Config.m_SvEmoteWheel ? GAME_VERSION_PLUS : GAME_VERSION; }
const char *CGameContext::NetVersion() const { return GAME_NETVERSION; }
const char *CGameContext::NetVersionHashUsed() const { return GAME_NETVERSION_HASH_FORCED; }
const char *CGameContext::NetVersionHashReal() const { return GAME_NETVERSION_HASH; }

void CGameContext::SendRoundStats()
{
	char aBuff[300];
	float BestKD = 0;
	float BestAccuracy = 0;
	int64_t BestKDPlayerIDs(0);
	int64_t BestAccuarcyPlayerIDs(0);
	
	for (int i = 0; i < MAX_CLIENTS; ++i)
	{
		CPlayer* p = m_apPlayers[i];
		if (!p || p->GetTeam() == TEAM_SPECTATORS)
			continue;
		SendChatTarget(i, "╔═════════ Statistics ═════════");
		SendChatTarget(i, "║");

		str_format(aBuff, 300, "║Kills(weapon): %d", p->m_Stats.m_Kills);
		SendChatTarget(i, aBuff);
		str_format(aBuff, 300, "║Hits(By opponent's weapon): %d", p->m_Stats.m_Hits);
		SendChatTarget(i, aBuff);
		SendChatTarget(i, "║");
		str_format(aBuff, 300, "║Kills/Deaths: %4.2f", (p->m_Stats.m_Hits != 0) ? (float)((float)p->m_Stats.m_Kills / (float)p->m_Stats.m_Hits) : (float)p->m_Stats.m_Kills);
		SendChatTarget(i, aBuff);		
		str_format(aBuff, 300, "║Shots | Kills/Shots: %d | %3.1f%%\n", p->m_Stats.m_Shots, ((float)p->m_Stats.m_Kills / (float)(p->m_Stats.m_Shots == 0 ? 1: p->m_Stats.m_Shots)) * 100.f);
		SendChatTarget(i, aBuff);
		SendChatTarget(i, "║");
		SendChatTarget(i, "╠══════════ Spikes ══════════");
		SendChatTarget(i, "║");
		str_format(aBuff, 300, "║Kills(normal spikes): %d", p->m_Stats.m_GrabsNormal);
		SendChatTarget(i, aBuff);
		str_format(aBuff, 300, "║Kills(team spikes): %d", p->m_Stats.m_GrabsTeam);
		SendChatTarget(i, aBuff);
		str_format(aBuff, 300, "║Kills(golden spikes): %d", p->m_Stats.m_GrabsGold);
		SendChatTarget(i, aBuff);
		str_format(aBuff, 300, "║Kills(false spikes): %d", p->m_Stats.m_GrabsFalse);
		SendChatTarget(i, aBuff);
		str_format(aBuff, 300, "║Spike deaths(while frozen): %d", p->m_Stats.m_Deaths);
		SendChatTarget(i, aBuff);
		SendChatTarget(i, "║");
		SendChatTarget(i, "╠═══════════ Misc ══════════");
		SendChatTarget(i, "║");
		str_format(aBuff, 300, "║Teammates hammered/unfrozen: %d / %d", p->m_Stats.m_UnfreezingHammerHits, p->m_Stats.m_Unfreezes);
		SendChatTarget(i, aBuff);
		SendChatTarget(i, "║");
		SendChatTarget(i, "╚══════════════════════════");
		SendChatTarget(i, "Press F1 to view stats now!!");

		float KDRatio = ((p->m_Stats.m_Hits != 0) ? (float)((float)p->m_Stats.m_Kills / (float)p->m_Stats.m_Hits) : (float)p->m_Stats.m_Kills);
		if (BestKD < KDRatio)
		{
			BestKD = KDRatio;
			BestKDPlayerIDs = 0;
			BestKDPlayerIDs |= CmaskOne(i);
		}
		else if (BestKD == KDRatio)
		{
			BestKDPlayerIDs |= CmaskOne(i);
		}

		float Accuracy = (float)p->m_Stats.m_Kills / (float)(p->m_Stats.m_Shots == 0 ? 1 : p->m_Stats.m_Shots);
		if (BestAccuracy < Accuracy)
		{
			BestAccuracy = Accuracy;
			BestAccuarcyPlayerIDs = 0;
			BestAccuarcyPlayerIDs |= CmaskOne(i);
		}
		else if (BestAccuracy == Accuracy)
		{
			BestAccuarcyPlayerIDs |= CmaskOne(i);
		}
	}

	int bestKDCount = CountBits(BestKDPlayerIDs);
	if (bestKDCount > 0)
	{
		char aBuff[300];
		if(bestKDCount == 1)
		{
			str_format(aBuff, 300, "Best player: %s with a K/D of %.3f", Server()->ClientName(PositionOfNonZeroBit(BestKDPlayerIDs, 0)), BestKD);
		}
		else
		{
			//only allow upto 10 players at once(else we risk buffer overflow)
			int CurPlayerCount = 0;
			int CurPlayerIDOffset = -1;
			
			char aPlayerNames[300];
			
			int CharacterOffset = 0;
			while((CurPlayerIDOffset = PositionOfNonZeroBit(BestKDPlayerIDs, CurPlayerIDOffset + 1)) != -1)
			{
				if(CurPlayerCount > 0)
				{					
					str_format((aPlayerNames + CharacterOffset), 300 - CharacterOffset,  ", ");
					CharacterOffset = str_length(aPlayerNames);
				}
				str_format((aPlayerNames + CharacterOffset), 300 - CharacterOffset, "%s", Server()->ClientName(CurPlayerIDOffset));
				CharacterOffset = str_length(aPlayerNames);
				++CurPlayerCount;
				
				if(CurPlayerCount > 10)
					break;
			}
			
			if(CurPlayerCount > 10)
				str_format((aPlayerNames + CharacterOffset), 300 - CharacterOffset, " and others");
			
			str_format(aBuff, 300, "Best players: %s with a K/D of %.3f", aPlayerNames, BestKD);
		}
		SendChat(-1, CHAT_ALL, -1, aBuff);
	}

	int BestAccuracyCount = CountBits(BestAccuarcyPlayerIDs);
	if (BestAccuracyCount > 0)
	{
		char aBuff[300];
		if (BestAccuracyCount == 1)
		{
			str_format(aBuff, 300, "Best accuracy: %s with %3.1f%%", Server()->ClientName(PositionOfNonZeroBit(BestKDPlayerIDs, 0)), BestAccuracy * 100.f);
		}
		else
		{
			//only allow upto 10 players at once(else we risk buffer overflow)
			int CurPlayerCount = 0;
			int CurPlayerIDOffset = -1;

			char aPlayerNames[300];

			int CharacterOffset = 0;
			while ((CurPlayerIDOffset = PositionOfNonZeroBit(BestKDPlayerIDs, CurPlayerIDOffset + 1)) != -1)
			{
				if (CurPlayerCount > 0)
				{
					str_format((aPlayerNames + CharacterOffset), 300 - CharacterOffset, ", ");
					CharacterOffset = str_length(aPlayerNames);
				}
				str_format((aPlayerNames + CharacterOffset), 300 - CharacterOffset, "%s", Server()->ClientName(CurPlayerIDOffset));
				CharacterOffset = str_length(aPlayerNames);
				++CurPlayerCount;

				if (CurPlayerCount > 10)
					break;
			}

			if (CurPlayerCount > 10)
				str_format((aPlayerNames + CharacterOffset), 300 - CharacterOffset, " and others");

			str_format(aBuff, 300, "Best accuracy: %s with %3.1f%%", aPlayerNames, BestAccuracy * 100.f);
		}
		SendChat(-1, CHAT_ALL, -1, aBuff);
	}
	
	if(g_Config.m_SvTrivia)
		SendRandomTrivia();
}

void CGameContext::SendRandomTrivia()
{	
	srand (time(NULL));
	int r = rand()%8;
	
	bool TriviaSent = false;
	
	//most jumps
	if(r == 0)
	{
		int MaxJumps = 0;
		int PlayerID = -1;
		
		for (int i = 0; i < MAX_CLIENTS; ++i)
		{
			CPlayer* p = m_apPlayers[i];
			if (!p || p->GetTeam() == TEAM_SPECTATORS)
				continue;
			if(MaxJumps < p->m_Stats.m_NumJumped)
			{
				MaxJumps = p->m_Stats.m_NumJumped;
				PlayerID = i;
			}
		}
		
		if(PlayerID != -1)
		{
			char aBuff[300];
			str_format(aBuff, sizeof(aBuff), "Trivia: %s jumped %d time%s in this round.", Server()->ClientName(PlayerID), MaxJumps, (MaxJumps == 1 ? "" : "s"));
			SendChat(-1, CHAT_ALL, -1, aBuff);
			TriviaSent = true;
		}
	}
	//longest travel distance
	else if(r == 1)
	{
		float MaxTilesMoved = 0.f;
		int PlayerID = -1;
		
		for (int i = 0; i < MAX_CLIENTS; ++i)
		{
			CPlayer* p = m_apPlayers[i];
			if (!p || p->GetTeam() == TEAM_SPECTATORS)
				continue;
			if(MaxTilesMoved < p->m_Stats.m_NumTilesMoved)
			{
				MaxTilesMoved = p->m_Stats.m_NumTilesMoved;
				PlayerID = i;
			}
		}
		
		if(PlayerID != -1)
		{
			char aBuff[300];
			str_format(aBuff, sizeof(aBuff), "Trivia: %s moved %5.2f tiles in this round.", Server()->ClientName(PlayerID), MaxTilesMoved/32.f);
			SendChat(-1, CHAT_ALL, -1, aBuff);
			TriviaSent = true;
		}
	}
	//most hooks
	else if(r == 2)
	{
		int MaxHooks = 0;
		int PlayerID = -1;
		
		for (int i = 0; i < MAX_CLIENTS; ++i)
		{
			CPlayer* p = m_apPlayers[i];
			if (!p || p->GetTeam() == TEAM_SPECTATORS)
				continue;
			if(MaxHooks < p->m_Stats.m_NumHooks)
			{
				MaxHooks = p->m_Stats.m_NumHooks;
				PlayerID = i;
			}
		}
		
		if(PlayerID != -1)
		{
			char aBuff[300];
			str_format(aBuff, sizeof(aBuff), "Trivia: %s hooked %d time%s in this round.", Server()->ClientName(PlayerID), MaxHooks, (MaxHooks == 1 ? "" : "s"));
			SendChat(-1, CHAT_ALL, -1, aBuff);
			TriviaSent = true;
		}
	}
	//fastest player
	else if(r == 3)
	{
		float MaxSpeed = 0.f;
		int PlayerID = -1;
		
		for (int i = 0; i < MAX_CLIENTS; ++i)
		{
			CPlayer* p = m_apPlayers[i];
			if (!p || p->GetTeam() == TEAM_SPECTATORS)
				continue;
			if(MaxSpeed < p->m_Stats.m_MaxSpeed)
			{
				MaxSpeed = p->m_Stats.m_MaxSpeed;
				PlayerID = i;
			}
		}
		
		if(PlayerID != -1)
		{
			char aBuff[300];
			str_format(aBuff, sizeof(aBuff), "Trivia: %s was the fastest player with %4.2f tiles per second(no fallspeed).", Server()->ClientName(PlayerID), (MaxSpeed*(float)Server()->TickSpeed())/32.f);
			SendChat(-1, CHAT_ALL, -1, aBuff);
			TriviaSent = true;
		}
	}
	//most bounces
	else if(r == 4)
	{
		int MaxTeeCols = 0;
		int PlayerID = -1;
		
		for (int i = 0; i < MAX_CLIENTS; ++i)
		{
			CPlayer* p = m_apPlayers[i];
			if (!p || p->GetTeam() == TEAM_SPECTATORS)
				continue;
			if(MaxTeeCols < p->m_Stats.m_NumTeeCollisions)
			{
				MaxTeeCols = p->m_Stats.m_NumTeeCollisions;
				PlayerID = i;
			}
		}
		
		if(PlayerID != -1)
		{
			char aBuff[300];
			str_format(aBuff, sizeof(aBuff), "Trivia: %s bounced %d time%s from other players.", Server()->ClientName(PlayerID), MaxTeeCols, (MaxTeeCols == 1 ? "" : "s"));
			SendChat(-1, CHAT_ALL, -1, aBuff);
			TriviaSent = true;
		}
	}
	//player longest freeze time
	else if(r == 5)
	{
		int MaxFreezeTicks = 0;
		int PlayerID = -1;
		
		for (int i = 0; i < MAX_CLIENTS; ++i)
		{
			CPlayer* p = m_apPlayers[i];
			if (!p || p->GetTeam() == TEAM_SPECTATORS)
				continue;
			if(MaxFreezeTicks < p->m_Stats.m_NumFreezeTicks)
			{
				MaxFreezeTicks = p->m_Stats.m_NumFreezeTicks;
				PlayerID = i;
			}
		}
		
		if(PlayerID != -1)
		{
			char aBuff[300];
			str_format(aBuff, sizeof(aBuff), "Trivia: %s was frozen for %4.2f seconds total this round.", Server()->ClientName(PlayerID), (float)MaxFreezeTicks/(float)Server()->TickSpeed());
			SendChat(-1, CHAT_ALL, -1, aBuff);
			TriviaSent = true;
		}
	}
	//player with most hammers to frozen teammates
	else if(r == 6)
	{
		int MaxUnfreezeHammers = 0;
		int PlayerID = -1;
		
		for (int i = 0; i < MAX_CLIENTS; ++i)
		{
			CPlayer* p = m_apPlayers[i];
			if (!p || p->GetTeam() == TEAM_SPECTATORS)
				continue;
			if(MaxUnfreezeHammers < p->m_Stats.m_UnfreezingHammerHits)
			{
				MaxUnfreezeHammers = p->m_Stats.m_UnfreezingHammerHits;
				PlayerID = i;
			}
		}
		
		if(PlayerID != -1)
		{
			char aBuff[300];
			str_format(aBuff, sizeof(aBuff), "Trivia: %s hammered %d frozen teammate%s.", Server()->ClientName(PlayerID), MaxUnfreezeHammers, (MaxUnfreezeHammers == 1 ? "" : "s"));
			SendChat(-1, CHAT_ALL, -1, aBuff);
			TriviaSent = true;
		}
	}
	//player with most emotes
	else if(r == 7)
	{
		int MaxEmotes = 0;
		int PlayerID = -1;
		
		for (int i = 0; i < MAX_CLIENTS; ++i)
		{
			CPlayer* p = m_apPlayers[i];
			if (!p || p->GetTeam() == TEAM_SPECTATORS)
				continue;
			if(MaxEmotes < p->m_Stats.m_NumEmotes)
			{
				MaxEmotes = p->m_Stats.m_NumEmotes;
				PlayerID = i;
			}
		}
		
		if(PlayerID != -1)
		{
			char aBuff[300];
			str_format(aBuff, sizeof(aBuff), "Trivia: %s emoted %d time%s.", Server()->ClientName(PlayerID), MaxEmotes, (MaxEmotes == 1 ? "" : "s"));
			SendChat(-1, CHAT_ALL, -1, aBuff);
			TriviaSent = true;
		}
	}
	//player that was hit most often
	else if(r == 8)
	{
		int MaxHit = 0;
		int PlayerID = -1;
		
		for (int i = 0; i < MAX_CLIENTS; ++i)
		{
			CPlayer* p = m_apPlayers[i];
			if (!p || p->GetTeam() == TEAM_SPECTATORS)
				continue;
			if(MaxHit < p->m_Stats.m_Hits)
			{
				MaxHit = p->m_Stats.m_Hits;
				PlayerID = i;
			}
		}
		
		if(PlayerID != -1)
		{
			char aBuff[300];
			str_format(aBuff, sizeof(aBuff), "Trivia: %s was hitted %d time%s by the opponent's weapon.", Server()->ClientName(PlayerID), MaxHit, (MaxHit == 1 ? "" : "s"));
			SendChat(-1, CHAT_ALL, -1, aBuff);
			TriviaSent = true;
		}
	}
	//player that was thrown into spikes most often by opponents
	else if(r == 9)
	{
		int MaxDeaths = 0;
		int PlayerID = -1;
		
		for (int i = 0; i < MAX_CLIENTS; ++i)
		{
			CPlayer* p = m_apPlayers[i];
			if (!p || p->GetTeam() == TEAM_SPECTATORS)
				continue;
			if(MaxDeaths < p->m_Stats.m_Deaths)
			{
				MaxDeaths = p->m_Stats.m_Deaths;
				PlayerID = i;
			}
		}
		
		if(PlayerID != -1)
		{
			char aBuff[300];
			str_format(aBuff, sizeof(aBuff), "Trivia: %s was thrown %d time%s into spikes by the opponents, while being frozen.", Server()->ClientName(PlayerID), MaxDeaths, (MaxDeaths == 1 ? "" : "s"));
			SendChat(-1, CHAT_ALL, -1, aBuff);
			TriviaSent = true;
		}
	}
	//player that threw most opponents into golden spikes
	else if(r == 10)
	{
		int MaxGold = 0;
		int PlayerID = -1;
		
		for (int i = 0; i < MAX_CLIENTS; ++i)
		{
			CPlayer* p = m_apPlayers[i];
			if (!p || p->GetTeam() == TEAM_SPECTATORS)
				continue;
			if(MaxGold < p->m_Stats.m_GrabsGold)
			{
				MaxGold = p->m_Stats.m_GrabsGold;
				PlayerID = i;
			}
		}
		
		if(PlayerID != -1)
		{
			char aBuff[300];
			str_format(aBuff, sizeof(aBuff), "Trivia: %s threw %d time%s frozen opponents into golden spikes.", Server()->ClientName(PlayerID), MaxGold, (MaxGold == 1 ? "" : "s"));
			SendChat(-1, CHAT_ALL, -1, aBuff);
			TriviaSent = true;
		}
		else
		{
			//send another trivia, bcs this is rare on maps without golden spikes
			SendRandomTrivia();			
			TriviaSent = true;
		}
	}
	
	if(!TriviaSent)
	{
		SendChat(-1, CHAT_ALL, -1, "Trivia: Press F1 and use PageUp and PageDown to scroll in the console window");
	}
}

IGameServer *CreateGameServer() { return new CGameContext; }