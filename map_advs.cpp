L#include <stdio.h>
#include "map_advs.h"
#include "metamod_oslink.h"
#include "entitykeyvalues.h"
#include "schemasystem/schemasystem.h"
#include <fstream>

map_advs g_map_advs;
PLUGIN_EXPOSE(map_advs, g_map_advs);
IVEngineServer2* engine = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem* g_pEntitySystem = nullptr;
CGlobalVars *gpGlobals = nullptr;

IUtilsApi* g_pUtils;
IMenusApi* g_pMenus;
IVIPApi* g_pVIPCore;

struct MapModels
{
	std::string sName;
	std::string sModel;
	Vector vPosition;
	QAngle qRotation;
	CHandle<CBaseProp> hEntity;
};

int g_iPropTemp[64];
std::vector<MapModels> g_mapEntities;

bool g_bTeleport[64];

std::map<std::string, std::string> g_Entities;
bool g_bVIPToggle[64];
bool g_bAccess[64];

SH_DECL_HOOK7_void(ISource2GameEntities, CheckTransmit, SH_NOATTRIB, 0, CCheckTransmitInfo **, int, CBitVec<16384> &, const Entity2Networkable_t **, const uint16 *, int, bool);

CGameEntitySystem* GameEntitySystem()
{
	return g_pUtils->GetCGameEntitySystem();
}

bool containsOnlyDigits(const std::string& str) {
	return str.find_first_not_of("0123456789") == std::string::npos;
}

CON_COMMAND_F(mm_map_advs_give, "", FCVAR_NONE)
{	
	if (args.ArgC() > 1 && args[1][0])
	{
		bool bFound = false;
		CCSPlayerController* pController;
		int iSlot = 0;
		for (int i = 0; i < 64; i++)
		{
			pController = CCSPlayerController::FromSlot(i);
			if (!pController)
				continue;
			uint32 m_steamID = pController->m_steamID();
			if(m_steamID == 0)
				continue;
			if(strstr(pController->m_iszPlayerName(), args[1]) || (containsOnlyDigits(args[1]) && m_steamID == std::stoll(args[1])) || (containsOnlyDigits(args[1]) && std::stoll(args[1]) == i) || (containsOnlyDigits(args[1]) && std::stoll(args[1]) == engine->GetClientXUID(i)))
			{
				bFound = true;
				iSlot = i;
				break;
			}
		}
		if(bFound)
		{
			g_bAccess[iSlot] = true;
		}
		else META_CONPRINT("[MAP_ADVS] Player not found\n");
	}
	else META_CONPRINT("[MAP_ADVS] Usage: mm_map_advs_give <userid|nickname|accountid>\n");
}

void LoadConfig()
{
	{
		KeyValues* g_kvSettings = new KeyValues("Models");
		const char *pszPath = "addons/configs/map_advs/map_advs.ini";

		if (!g_kvSettings->LoadFromFile(g_pFullFileSystem, pszPath))
		{
			g_pUtils->ErrorLog("[%s] Failed to load %s\n", g_PLAPI->GetLogTag(), pszPath);
			return;
		}
		g_Entities.clear();
		FOR_EACH_VALUE(g_kvSettings, pValue)
		{
			g_Entities[std::string(pValue->GetName())] = std::string(pValue->GetString(nullptr, nullptr));
		}
	}
	
	{
		KeyValues* g_kvSettings = new KeyValues("Models");
		char szPath[256];
		g_SMAPI->Format(szPath, sizeof(szPath), "addons/configs/map_advs/props_%s.ini", g_pUtils->GetCGlobalVars()->mapname);

		if (!g_kvSettings->LoadFromFile(g_pFullFileSystem, szPath))
		{
			char szPath2[256];
			g_SMAPI->Format(szPath2, sizeof(szPath2), "%s/%s", g_SMAPI->GetBaseDir(), szPath);
			std::fstream file;
			file.open(szPath2, std::fstream::out | std::fstream::trunc);
			file << "\"Models\"\n{\n\n}\n";
			file.close();
			return;
		}
		g_mapEntities.clear();
		FOR_EACH_SUBKEY(g_kvSettings, pValue)
		{
			Vector vPosition(pValue->GetFloat("position_x"), pValue->GetFloat("position_y"), pValue->GetFloat("position_z"));
			QAngle qRotation(pValue->GetFloat("rotation_x"), pValue->GetFloat("rotation_y"), pValue->GetFloat("rotation_z"));
			g_mapEntities.push_back({pValue->GetString("name"), pValue->GetString("model"), vPosition, qRotation});
		}
	}
}

void StartupServer()
{
	g_pGameEntitySystem = GameEntitySystem();
	g_pEntitySystem = g_pUtils->GetCEntitySystem();
	gpGlobals = g_pUtils->GetCGlobalVars();
	LoadConfig();
	for(int i = 0; i < 64; i++)
	{
		g_bTeleport[i] = false;
		g_bAccess[i] = false;
	}
}

bool map_advs::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pSource2GameEntities, ISource2GameEntities, SOURCE2GAMEENTITIES_INTERFACE_VERSION);

	SH_ADD_HOOK(ISource2GameEntities, CheckTransmit, g_pSource2GameEntities, SH_MEMBER(this, &map_advs::Hook_CheckTransmit), true);

	g_SMAPI->AddListener( this, this );
	ConVar_Register(FCVAR_GAMEDLL);

	return true;
}

void map_advs::Hook_CheckTransmit(CCheckTransmitInfo **ppInfoList, int infoCount, CBitVec<16384> &unionTransmitEdicts, const Entity2Networkable_t **pNetworkables, const uint16 *pEntityIndicies, int nEntities, bool bEnablePVSBits)
{
	if (!g_pEntitySystem || !g_pVIPCore)
		return;

	for (int i = 0; i < infoCount; i++)
	{
		auto &pInfo = ppInfoList[i];
		int iPlayerSlot = (int)*((uint8 *)pInfo + 584);
		CCSPlayerController* pSelfController = CCSPlayerController::FromSlot(iPlayerSlot);
		if (!pSelfController || !pSelfController->IsConnected())
			continue;
		
		if(g_bVIPToggle[iPlayerSlot])
		{
			for(auto& model : g_mapEntities)
			{
				if(model.hEntity)
					pInfo->m_pTransmitEntity->Clear(model.hEntity->entindex());
			}
		}
	}
}

bool map_advs::Unload(char *error, size_t maxlen)
{
	SH_REMOVE_HOOK(ISource2GameEntities, CheckTransmit, g_pSource2GameEntities, SH_MEMBER(this, &map_advs::Hook_CheckTransmit), true);
	ConVar_Unregister();
	
	return true;
}

CBaseProp* CreateEntity(MapModels& model)
{
	CBaseProp* pEntity = dynamic_cast<CBaseProp*>(g_pUtils->CreateEntityByName("prop_dynamic", -1));
	if (!pEntity)
	{
		Warning("Failed to create entity %s\n", model.sModel.c_str());
		return nullptr;
	}
	g_pUtils->TeleportEntity(pEntity, &model.vPosition, &model.qRotation, nullptr);
	CEntityKeyValues* pKeyValues = new CEntityKeyValues();
	pKeyValues->SetString("model", model.sModel.c_str());
	g_pUtils->DispatchSpawn(pEntity, pKeyValues);
	return pEntity;
}

void RemoveEntity(MapModels& model)
{
	if (model.hEntity)
	{
		g_pUtils->RemoveEntity(model.hEntity);
		model.hEntity = nullptr;
	}
}

void DeleteData(int iIndex)
{
	KeyValues* g_kvSettings = new KeyValues("Models");
	char szPath[512];
	g_SMAPI->Format(szPath, sizeof(szPath), "addons/configs/map_advs/props_%s.ini", g_pUtils->GetCGlobalVars()->mapname);

	if (!g_kvSettings->LoadFromFile(g_pFullFileSystem, szPath))
	{
		Warning("Failed to load %s\n", szPath);
		return;
	}
	MapModels entity = g_mapEntities[iIndex];
	char sCookieName[64];
	g_SMAPI->Format(sCookieName, sizeof(sCookieName), "model_%d", iIndex);
	KeyValues *hData = g_kvSettings->FindKey(sCookieName, false);
	if(hData)
		g_kvSettings->RemoveSubKey(hData, true, true);
	g_kvSettings->SaveToFile(g_pFullFileSystem, szPath);
	delete g_kvSettings;
}

void SaveData(int iIndex)
{
	KeyValues* g_kvSettings = new KeyValues("Models");
	char szPath[512];
	g_SMAPI->Format(szPath, sizeof(szPath), "addons/configs/map_advs/props_%s.ini", g_pUtils->GetCGlobalVars()->mapname);

	if (!g_kvSettings->LoadFromFile(g_pFullFileSystem, szPath))
	{
		Warning("Failed to load %s\n", szPath);
		return;
	}
	MapModels entity = g_mapEntities[iIndex];
	char sCookieName[64];
	g_SMAPI->Format(sCookieName, sizeof(sCookieName), "model_%d", iIndex);
	KeyValues *hData = g_kvSettings->FindKey(sCookieName, true);
	hData->SetString("name", entity.sName.c_str());
	hData->SetString("model", entity.sModel.c_str());
	hData->SetFloat("position_x", entity.vPosition.x);
	hData->SetFloat("position_y", entity.vPosition.y);
	hData->SetFloat("position_z", entity.vPosition.z);
	hData->SetFloat("rotation_x", entity.qRotation.x);
	hData->SetFloat("rotation_y", entity.qRotation.y);
	hData->SetFloat("rotation_z", entity.qRotation.z);
	g_kvSettings->SaveToFile(g_pFullFileSystem, szPath);
	delete g_kvSettings;
}

void OnRoundStart(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	for (auto& model : g_mapEntities)
	{
		model.hEntity = CHandle<CBaseProp>(CreateEntity(model));
	}
}

void MainMenu(int iSlot);
void ShowList(int iSlot);
void ShowAdd(int iSlot);
void ShowItem(int iIndex, int iSlot);

void OnPlayerPing(const char* szName, IGameEvent* pEvent, bool bDontBroadcast)
{
	int iSlot = pEvent->GetInt("userid");
	if(g_bTeleport[iSlot])
	{
		int iIndex = g_iPropTemp[iSlot];
		g_mapEntities[iIndex].vPosition = Vector(pEvent->GetFloat("x"), pEvent->GetFloat("y"), pEvent->GetFloat("z"));
		if(g_mapEntities[iIndex].hEntity)
			g_pUtils->TeleportEntity(g_mapEntities[iIndex].hEntity, &g_mapEntities[iIndex].vPosition, &g_mapEntities[iIndex].qRotation, nullptr);
		else
			g_mapEntities[iIndex].hEntity = CHandle<CBaseProp>(CreateEntity(g_mapEntities[iIndex]));
		SaveData(iIndex);
		g_bTeleport[iSlot] = false;
		ShowItem(iIndex, iSlot);
	}
}

void ShowAdd(int iSlot)
{
	Menu hMenu;
	g_pMenus->SetTitleMenu(hMenu, "Добавить рекламу");
	for(auto& entity : g_Entities)
	{
		g_pMenus->AddItemMenu(hMenu, entity.first.c_str(), entity.first.c_str());
	}
	g_pMenus->SetExitMenu(hMenu, true);
	g_pMenus->SetBackMenu(hMenu, true);
	g_pMenus->SetCallback(hMenu, [](const char* szBack, const char* szFront, int iItem, int iSlot)
	{
		if(iItem < 7)
		{
			CCSPlayerController* pPlayer = CCSPlayerController::FromSlot(iSlot);
			if(!pPlayer) return;
			CCSPlayerPawn* pPawn = pPlayer->GetPlayerPawn();
			if(!pPawn || !pPawn->IsAlive()) return;
			MapModels model;
			model.sName = szBack;
			model.sModel = g_Entities[szBack];
			model.vPosition = Vector(0, 0, 0);
			model.qRotation = pPlayer->GetAbsRotation();
			g_mapEntities.push_back(model);
			g_bTeleport[iSlot] = true;
			g_iPropTemp[iSlot] = g_mapEntities.size() - 1;
			g_pUtils->PrintToChat(iSlot, "Выберите место для рекламы с помощью пинга(колёсика мышки)");
			g_pMenus->ClosePlayerMenu(iSlot);
		}
		else if(iItem == 7)
			MainMenu(iSlot);
	});
	g_pMenus->DisplayPlayerMenu(hMenu, iSlot);
}

void ShowItem(int iIndex, int iSlot)
{
	Menu hMenu;
	g_pMenus->SetTitleMenu(hMenu, g_mapEntities[iIndex].sName.c_str());
	g_pMenus->AddItemMenu(hMenu, "move", "Двигать");
	g_pMenus->AddItemMenu(hMenu, "rotate", "Поворачивать");
	g_pMenus->AddItemMenu(hMenu, "teleport", "Телепортироваться");
	g_pMenus->AddItemMenu(hMenu, "teleport2", "Телепортировать пингом");
	g_pMenus->AddItemMenu(hMenu, "delete", "Удалить");
	g_pMenus->SetExitMenu(hMenu, true);
	g_pMenus->SetBackMenu(hMenu, true);
	g_pMenus->SetCallback(hMenu, [iIndex](const char* szBack, const char* szFront, int iItem, int iSlot)
	{
		if(iItem < 7)
		{
			if(!strcmp(szBack, "move"))
			{
				Menu hMenu;
				g_pMenus->SetTitleMenu(hMenu, "Движение");
				g_pMenus->AddItemMenu(hMenu, "x;10", "По оси X +10");
				g_pMenus->AddItemMenu(hMenu, "x;-10", "По оси X -10");
				g_pMenus->AddItemMenu(hMenu, "y;10", "По оси Y +10");
				g_pMenus->AddItemMenu(hMenu, "y;-10", "По оси Y -10");
				g_pMenus->AddItemMenu(hMenu, "z;10", "По оси Z +10");
				g_pMenus->AddItemMenu(hMenu, "z;-10", "По оси Z -10");
				g_pMenus->SetExitMenu(hMenu, true);
				g_pMenus->SetBackMenu(hMenu, true);
				g_pMenus->SetCallback(hMenu, [iIndex](const char* szBack, const char* szFront, int iItem, int iSlot)
				{
					if(iItem < 7)
					{
						Vector vPosition = g_mapEntities[iIndex].vPosition;
						if(!strcmp(szBack, "x;10"))
						{
							vPosition.x += 10;
						}
						else if(!strcmp(szBack, "x;-10"))
						{
							vPosition.x -= 10;
						}
						else if(!strcmp(szBack, "y;10"))
						{
							vPosition.y += 10;
						}
						else if(!strcmp(szBack, "y;-10"))
						{
							vPosition.y -= 10;
						}
						else if(!strcmp(szBack, "z;10"))
						{
							vPosition.z += 10;
						}
						else if(!strcmp(szBack, "z;-10"))
						{
							vPosition.z -= 10;
						}
						g_mapEntities[iIndex].vPosition = vPosition;
						if(g_mapEntities[iIndex].hEntity)
							g_pUtils->TeleportEntity(g_mapEntities[iIndex].hEntity, &vPosition, &g_mapEntities[iIndex].qRotation, nullptr);
						SaveData(iIndex);
					}
					else if(iItem == 7)
					{
						ShowItem(iIndex, iSlot);
					}
				});
				g_pMenus->DisplayPlayerMenu(hMenu, iSlot);
			}
			else if(!strcmp(szBack, "rotate"))
			{
				Menu hMenu;
				g_pMenus->SetTitleMenu(hMenu, "Поворот");
				g_pMenus->AddItemMenu(hMenu, "x;10", "По оси X +10");
				g_pMenus->AddItemMenu(hMenu, "x;-10", "По оси X -10");
				g_pMenus->AddItemMenu(hMenu, "y;10", "По оси Y +10");
				g_pMenus->AddItemMenu(hMenu, "y;-10", "По оси Y -10");
				g_pMenus->AddItemMenu(hMenu, "z;10", "По оси Z +10");
				g_pMenus->AddItemMenu(hMenu, "z;-10", "По оси Z -10");
				g_pMenus->SetExitMenu(hMenu, true);
				g_pMenus->SetBackMenu(hMenu, true);
				g_pMenus->SetCallback(hMenu, [iIndex](const char* szBack, const char* szFront, int iItem, int iSlot)
				{
					if(iItem < 7)
					{
						QAngle qRotation = g_mapEntities[iIndex].qRotation;
						if(!strcmp(szBack, "x;10"))
						{
							qRotation.x += 10;
						}
						else if(!strcmp(szBack, "x;-10"))
						{
							qRotation.x -= 10;
						}
						else if(!strcmp(szBack, "y;10"))
						{
							qRotation.y += 10;
						}
						else if(!strcmp(szBack, "y;-10"))
						{
							qRotation.y -= 10;
						}
						else if(!strcmp(szBack, "z;10"))
						{
							qRotation.z += 10;
						}
						else if(!strcmp(szBack, "z;-10"))
						{
							qRotation.z -= 10;
						}
						g_mapEntities[iIndex].qRotation = qRotation;
						if(g_mapEntities[iIndex].hEntity)
							g_pUtils->TeleportEntity(g_mapEntities[iIndex].hEntity, &g_mapEntities[iIndex].vPosition, &qRotation, nullptr);
						SaveData(iIndex);
					}
					else if(iItem == 7)
					{
						ShowItem(iIndex, iSlot);
					}
				});
				g_pMenus->DisplayPlayerMenu(hMenu, iSlot);
			}
			else if(!strcmp(szBack, "teleport"))
			{
				CCSPlayerController* pPlayer = CCSPlayerController::FromSlot(iSlot);
				if(!pPlayer || !pPlayer->GetPlayerPawn() || !pPlayer->GetPlayerPawn()->IsAlive())\
				{
					g_pUtils->PrintToChat(iSlot, "Для этого вы должны быть живы");
					return;
				}
				g_pUtils->TeleportEntity(pPlayer->GetPlayerPawn(), &g_mapEntities[iIndex].vPosition, &g_mapEntities[iIndex].qRotation, nullptr);
			}
			else if(!strcmp(szBack, "teleport2"))
			{
				g_bTeleport[iSlot] = true;
				g_iPropTemp[iSlot] = iIndex;
				g_pUtils->PrintToChat(iSlot, "Выберите место для рекламы с помощью пинга(колёсика мышки)");
				g_pMenus->ClosePlayerMenu(iSlot);
			}
			else if(!strcmp(szBack, "delete"))
			{
				RemoveEntity(g_mapEntities[iIndex]);
				DeleteData(iIndex);
				g_mapEntities.erase(g_mapEntities.begin() + iIndex);
				ShowList(iSlot);
			}
		}
		else if(iItem == 7)
			ShowList(iSlot);
	});
	g_pMenus->DisplayPlayerMenu(hMenu, iSlot);
}

void ShowList(int iSlot)
{
	Menu hMenu;
	g_pMenus->SetTitleMenu(hMenu, "Список рекламы");
	for (size_t i = 0; i < g_mapEntities.size(); i++)
	{
		g_pMenus->AddItemMenu(hMenu, std::to_string(i).c_str(), g_mapEntities[i].sName.c_str());
	}
	g_pMenus->SetExitMenu(hMenu, true);
	g_pMenus->SetBackMenu(hMenu, true);
	g_pMenus->SetCallback(hMenu, [](const char* szBack, const char* szFront, int iItem, int iSlot)
	{
		if(iItem < 7)
		{
			int iIndex = atoi(szBack);
			ShowItem(iIndex, iSlot);
		}
		else if(iItem == 7)
			MainMenu(iSlot);
	});
	g_pMenus->DisplayPlayerMenu(hMenu, iSlot);
}

void MainMenu(int iSlot)
{
	Menu hMenu;
	g_pMenus->SetTitleMenu(hMenu, "Реклама");
	g_pMenus->AddItemMenu(hMenu, "list", "Список рекламы");
	g_pMenus->AddItemMenu(hMenu, "add", "Добавить рекламу");
	g_pMenus->SetExitMenu(hMenu, true);
	g_pMenus->SetCallback(hMenu, [](const char* szBack, const char* szFront, int iItem, int iSlot)
	{
		if(iItem < 7)
		{
			if(!strcmp(szBack, "list"))
				ShowList(iSlot);
			else if(!strcmp(szBack, "add"))
				ShowAdd(iSlot);
		}
	});
	g_pMenus->DisplayPlayerMenu(hMenu, iSlot);
}

bool OnMapAdvs(int iSlot, const char* szContent)
{
	if(g_bAccess[iSlot])
	{
		MainMenu(iSlot);
	}
	else
		g_pUtils->PrintToChat(iSlot, "У вас нет доступа");
	return true;
}

bool OnToggle(int iSlot, const char* szFeature, VIP_ToggleState eOldStatus, VIP_ToggleState& eNewStatus)
{
	g_bVIPToggle[iSlot] = eNewStatus == ENABLED;
	return false;
}

void map_advs::AllPluginsLoaded()
{
	char error[64];
	int ret;
	g_pUtils = (IUtilsApi *)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		g_SMAPI->Format(error, sizeof(error), "Missing Utils system plugin");
		ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}
	g_pMenus = (IMenusApi*)g_SMAPI->MetaFactory(Menus_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
	{
		char error[64];
		V_strncpy(error, "Failed to lookup menus api. Aborting", 64);
		ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), error);
		std::string sBuffer = "meta unload "+std::to_string(g_PLID);
		engine->ServerCommand(sBuffer.c_str());
		return;
	}
	g_pVIPCore = (IVIPApi*)g_SMAPI->MetaFactory(VIP_INTERFACE, &ret, NULL);
	if (ret == META_IFACE_FAILED)
		g_pVIPCore = nullptr;
	else
	{
		g_pVIPCore->VIP_RegisterFeature("map_advs", VIP_BOOL, TOGGLABLE, nullptr, OnToggle);
		g_pVIPCore->VIP_OnClientLoaded([](int iSlot, bool bIsVIP)
		{
			g_bVIPToggle[iSlot] = g_pVIPCore->VIP_GetClientFeatureBool(iSlot, "map_advs");
		});
	}
	g_pUtils->StartupServer(g_PLID, StartupServer);
	g_pUtils->HookEvent(g_PLID, "round_start", OnRoundStart);
	g_pUtils->HookEvent(g_PLID, "player_ping", OnPlayerPing);
	g_pUtils->RegCommand(g_PLID, {"mm_map_advs"}, {"!map_advs"}, OnMapAdvs);
}

///////////////////////////////////////
const char* map_advs::GetLicense()
{
	return "GPL";
}

const char* map_advs::GetVersion()
{
	return "1.0";
}

const char* map_advs::GetDate()
{
	return __DATE__;
}

const char *map_advs::GetLogTag()
{
	return "map_advs";
}

const char* map_advs::GetAuthor()
{
	return "Pisex";
}

const char* map_advs::GetDescription()
{
	return "Map Advertisements";
}

const char* map_advs::GetName()
{
	return "Map Advertisements";
}

const char* map_advs::GetURL()
{
	return "https://discord.gg/g798xERK5Y";
}
