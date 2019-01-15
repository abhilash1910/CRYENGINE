// Copyright 2001-2018 Crytek GmbH / Crytek Group. All rights reserved.

#include "stdafx.h"
#include "Impl.h"

#include "CVars.h"
#include "Listener.h"
#include "Object.h"
#include "GlobalObject.h"
#include "File.h"
#include "Event.h"
#include "Trigger.h"
#include "Parameter.h"
#include "SwitchState.h"
#include "Environment.h"
#include "Setting.h"
#include "StandaloneFile.h"
#include "IoInterface.h"

#include <FileInfo.h>
#include <Logger.h>
#include <CrySystem/IStreamEngine.h>
#include <CrySystem/File/ICryPak.h>
#include <CrySystem/IProjectManager.h>
#include <CryAudio/IAudioSystem.h>

#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
	#include <DebugStyle.h>
	#include <CryRenderer/IRenderAuxGeom.h>
#endif  // INCLUDE_ADX2_IMPL_PRODUCTION_CODE

#if defined(CRY_PLATFORM_WINDOWS)
	#include <cri_atom_wasapi.h>
#endif // CRY_PLATFORM_WINDOWS

namespace CryAudio
{
namespace Impl
{
namespace Adx2
{
std::vector<CBaseObject*> g_constructedObjects;
SPoolSizes g_poolSizes;
SPoolSizes g_poolSizesLevelSpecific;

#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
SPoolSizes g_debugPoolSizes;
Events g_constructedEvents;
uint16 g_objectPoolSize = 0;
uint16 g_eventPoolSize = 0;
#endif  // INCLUDE_ADX2_IMPL_PRODUCTION_CODE

//////////////////////////////////////////////////////////////////////////
void CountPoolSizes(XmlNodeRef const pNode, SPoolSizes& poolSizes)
{
	uint16 numTriggers = 0;
	pNode->getAttr(s_szTriggersAttribute, numTriggers);
	poolSizes.triggers += numTriggers;

	uint16 numParameters = 0;
	pNode->getAttr(s_szParametersAttribute, numParameters);
	poolSizes.parameters += numParameters;

	uint16 numSwitchStates = 0;
	pNode->getAttr(s_szSwitchStatesAttribute, numSwitchStates);
	poolSizes.switchStates += numSwitchStates;

	uint16 numEnvironments = 0;
	pNode->getAttr(s_szEnvironmentsAttribute, numEnvironments);
	poolSizes.environments += numEnvironments;

	uint16 numSettings = 0;
	pNode->getAttr(s_szSettingsAttribute, numSettings);
	poolSizes.settings += numSettings;

	uint16 numFiles = 0;
	pNode->getAttr(s_szFilesAttribute, numFiles);
	poolSizes.files += numFiles;
}

//////////////////////////////////////////////////////////////////////////
void AllocateMemoryPools(uint16 const objectPoolSize, uint16 const eventPoolSize)
{
	CObject::CreateAllocator(objectPoolSize);
	CEvent::CreateAllocator(eventPoolSize);
	CTrigger::CreateAllocator(g_poolSizes.triggers);
	CParameter::CreateAllocator(g_poolSizes.parameters);
	CSwitchState::CreateAllocator(g_poolSizes.switchStates);
	CEnvironment::CreateAllocator(g_poolSizes.environments);
	CSetting::CreateAllocator(g_poolSizes.settings);
	CFile::CreateAllocator(g_poolSizes.files);
}

//////////////////////////////////////////////////////////////////////////
void FreeMemoryPools()
{
	CObject::FreeMemoryPool();
	CEvent::FreeMemoryPool();
	CTrigger::FreeMemoryPool();
	CParameter::FreeMemoryPool();
	CSwitchState::FreeMemoryPool();
	CEnvironment::FreeMemoryPool();
	CSetting::FreeMemoryPool();
	CFile::FreeMemoryPool();
}

#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
std::map<string, std::vector<std::pair<string, float>>> g_cueRadiusInfo;

//////////////////////////////////////////////////////////////////////////
void ParseAcbInfoFile(XmlNodeRef const pRoot, string const& acbName)
{
	int const numChildren = pRoot->getChildCount();

	for (int i = 0; i < numChildren; ++i)
	{
		XmlNodeRef const pChild = pRoot->getChild(i);

		if (pChild != nullptr)
		{
			char const* const typeAttrib = pChild->getAttr("OrcaType");

			if (_stricmp(typeAttrib, "CriMw.CriAtomCraft.AcCore.AcOoCueSynthCue") == 0)
			{
				if (pChild->haveAttr("Pos3dDistanceMax"))
				{
					float distanceMax = 0.0f;
					pChild->getAttr("Pos3dDistanceMax", distanceMax);

					g_cueRadiusInfo[pChild->getAttr("OrcaName")].emplace_back(acbName, distanceMax);
				}
			}
			else if ((_stricmp(typeAttrib, "CriMw.CriAtomCraft.AcCore.AcOoCueFolder") == 0) ||
			         (_stricmp(typeAttrib, "CriMw.CriAtomCraft.AcCore.AcOoCueFolderPrivate") == 0))
			{
				ParseAcbInfoFile(pChild, acbName);
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////////
void LoadAcbInfos(string const& folderPath)
{
	_finddata_t fd;
	ICryPak* const pCryPak = gEnv->pCryPak;
	intptr_t const handle = pCryPak->FindFirst(folderPath + "/*_acb_info.xml", &fd);

	if (handle != -1)
	{
		do
		{
			string fileName = fd.name;
			XmlNodeRef const pRoot = GetISystem()->LoadXmlFromFile(folderPath + "/" + fileName);

			if (pRoot != nullptr)
			{
				XmlNodeRef const pInfoNode = pRoot->getChild(0);

				if (pInfoNode != nullptr)
				{
					XmlNodeRef const pInfoRootNode = pInfoNode->getChild(0);

					if (pInfoRootNode != nullptr)
					{
						XmlNodeRef const pAcbNode = pInfoRootNode->getChild(0);

						if ((pAcbNode != nullptr) && pAcbNode->haveAttr("AwbHash"))
						{
							ParseAcbInfoFile(pAcbNode, pAcbNode->getAttr("OrcaName"));
						}
					}
				}
			}
		}
		while (pCryPak->FindNext(handle, &fd) >= 0);

		pCryPak->FindClose(handle);
	}
}

//////////////////////////////////////////////////////////////////////////
static void errorCallback(Char8 const* const errid, Uint32 const p1, Uint32 const p2, Uint32* const parray)
{
	Char8 const* errorMessage;
	errorMessage = criErr_ConvertIdToMessage(errid, p1, p2);
	Cry::Audio::Log(ELogType::Error, static_cast<char const*>(errorMessage));
}
#endif // INCLUDE_ADX2_IMPL_PRODUCTION_CODE

//////////////////////////////////////////////////////////////////////////
CriError selectIoFunc(CriChar8 const* szPath, CriFsDeviceId* pDeviceId, CriFsIoInterfacePtr* pIoInterface)
{
	CRY_ASSERT_MESSAGE(szPath != nullptr, "szPath is null pointer during %s", __FUNCTION__);
	CRY_ASSERT_MESSAGE(pDeviceId != nullptr, "pDeviceId is null pointer during %s", __FUNCTION__);
	CRY_ASSERT_MESSAGE(pIoInterface != nullptr, "pIoInterface is null pointer during %s", __FUNCTION__);

	*pDeviceId = CRIFS_DEFAULT_DEVICE;
	*pIoInterface = &g_IoInterface;

	return CRIERR_OK;
}
//////////////////////////////////////////////////////////////////////////
void* userMalloc(void* const pObj, CriUint32 const size)
{
	MEMSTAT_CONTEXT(EMemStatContextTypes::MSC_AudioImpl, 0, "CryAudio::Impl::Adx2::userMalloc");
	void* const pMem = CryModuleMalloc(static_cast<size_t>(size));
	return pMem;
}

//////////////////////////////////////////////////////////////////////////
void userFree(void* const pObj, void* const pMem)
{
	CryModuleFree(pMem);
}

///////////////////////////////////////////////////////////////////////////
CImpl::CImpl()
	: m_pAcfBuffer(nullptr)
	, m_dbasId(CRIATOMEXDBAS_ILLEGAL_ID)
#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
	, m_name("Adx2 (" CRI_ATOM_VER_NUM ")")
#endif  // INCLUDE_ADX2_IMPL_PRODUCTION_CODE
{
	g_pImpl = this;
}

///////////////////////////////////////////////////////////////////////////
ERequestStatus CImpl::Init(uint16 const objectPoolSize)
{
	ERequestStatus result = ERequestStatus::Success;

	if (g_cvars.m_eventPoolSize < 1)
	{
		g_cvars.m_eventPoolSize = 1;
		Cry::Audio::Log(ELogType::Warning, R"(Event pool size must be at least 1. Forcing the cvar "s_Adx2EventPoolSize" to 1!)");
	}

	g_constructedObjects.reserve(static_cast<size_t>(objectPoolSize));
	AllocateMemoryPools(objectPoolSize, static_cast<uint16>(g_cvars.m_eventPoolSize));

	m_regularSoundBankFolder = AUDIO_SYSTEM_DATA_ROOT;
	m_regularSoundBankFolder += "/";
	m_regularSoundBankFolder += s_szImplFolderName;
	m_regularSoundBankFolder += "/";
	m_regularSoundBankFolder += s_szAssetsFolderName;
	m_localizedSoundBankFolder = m_regularSoundBankFolder;

#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
	g_constructedEvents.reserve(static_cast<size_t>(g_cvars.m_eventPoolSize));

	g_objectPoolSize = objectPoolSize;
	g_eventPoolSize = static_cast<uint16>(g_cvars.m_eventPoolSize);

	LoadAcbInfos(m_regularSoundBankFolder);

	if (ICVar* pCVar = gEnv->pConsole->GetCVar("g_languageAudio"))
	{
		SetLanguage(pCVar->GetString());
		LoadAcbInfos(m_localizedSoundBankFolder);
	}

	criErr_SetCallback(errorCallback);
#endif  // INCLUDE_ADX2_IMPL_PRODUCTION_CODE

	criAtomEx_SetUserAllocator(userMalloc, userFree, nullptr);

	InitializeFileSystem();

	if (InitializeLibrary() && AllocateVoicePool() && CreateDbas() && RegisterAcf())
	{
		SetListenerConfig();
		SetPlayerConfig();
		Set3dSourceConfig();
	}
	else
	{
		ShutDown();
		result = ERequestStatus::Failure;
	}

	return result;
}

///////////////////////////////////////////////////////////////////////////
void CImpl::ShutDown()
{
	criAtomEx_UnregisterAcf();
	criAtomExDbas_Destroy(m_dbasId);
	criAtomExVoicePool_FreeAll();

#if defined(CRY_PLATFORM_WINDOWS)
	criAtomEx_Finalize_WASAPI();
#else
	criAtomEx_Finalize();
#endif  // CRY_PLATFORM_WINDOWS

	criFs_FinalizeLibrary();

	delete[] m_pAcfBuffer;
}

///////////////////////////////////////////////////////////////////////////
void CImpl::Release()
{
	delete this;
	g_pImpl = nullptr;
	g_cvars.UnregisterVariables();

	FreeMemoryPools();
}

//////////////////////////////////////////////////////////////////////////
void CImpl::SetLibraryData(XmlNodeRef const pNode, bool const isLevelSpecific)
{
	if (isLevelSpecific)
	{
		SPoolSizes levelPoolSizes;
		CountPoolSizes(pNode, levelPoolSizes);

		g_poolSizesLevelSpecific.triggers = std::max(g_poolSizesLevelSpecific.triggers, levelPoolSizes.triggers);
		g_poolSizesLevelSpecific.parameters = std::max(g_poolSizesLevelSpecific.parameters, levelPoolSizes.parameters);
		g_poolSizesLevelSpecific.switchStates = std::max(g_poolSizesLevelSpecific.switchStates, levelPoolSizes.switchStates);
		g_poolSizesLevelSpecific.environments = std::max(g_poolSizesLevelSpecific.environments, levelPoolSizes.environments);
		g_poolSizesLevelSpecific.settings = std::max(g_poolSizesLevelSpecific.settings, levelPoolSizes.settings);
		g_poolSizesLevelSpecific.files = std::max(g_poolSizesLevelSpecific.files, levelPoolSizes.files);
	}
	else
	{
		CountPoolSizes(pNode, g_poolSizes);
	}
}

//////////////////////////////////////////////////////////////////////////
void CImpl::OnBeforeLibraryDataChanged()
{
	ZeroStruct(g_poolSizes);
	ZeroStruct(g_poolSizesLevelSpecific);

#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
	ZeroStruct(g_debugPoolSizes);
#endif  // INCLUDE_ADX2_IMPL_PRODUCTION_CODE
}

//////////////////////////////////////////////////////////////////////////
void CImpl::OnAfterLibraryDataChanged()
{
	g_poolSizes.triggers += g_poolSizesLevelSpecific.triggers;
	g_poolSizes.parameters += g_poolSizesLevelSpecific.parameters;
	g_poolSizes.switchStates += g_poolSizesLevelSpecific.switchStates;
	g_poolSizes.environments += g_poolSizesLevelSpecific.environments;
	g_poolSizes.settings += g_poolSizesLevelSpecific.settings;
	g_poolSizes.files += g_poolSizesLevelSpecific.files;

#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
	// Used to hide pools without allocations in debug draw.
	g_debugPoolSizes = g_poolSizes;
#endif  // INCLUDE_ADX2_IMPL_PRODUCTION_CODE

	g_poolSizes.triggers = std::max<uint16>(1, g_poolSizes.triggers);
	g_poolSizes.parameters = std::max<uint16>(1, g_poolSizes.parameters);
	g_poolSizes.switchStates = std::max<uint16>(1, g_poolSizes.switchStates);
	g_poolSizes.environments = std::max<uint16>(1, g_poolSizes.environments);
	g_poolSizes.settings = std::max<uint16>(1, g_poolSizes.settings);
	g_poolSizes.files = std::max<uint16>(1, g_poolSizes.files);
}

///////////////////////////////////////////////////////////////////////////
void CImpl::OnLoseFocus()
{
	MuteAllObjects(CRI_TRUE);
}

///////////////////////////////////////////////////////////////////////////
void CImpl::OnGetFocus()
{
	MuteAllObjects(CRI_FALSE);
}

///////////////////////////////////////////////////////////////////////////
void CImpl::MuteAll()
{
	MuteAllObjects(CRI_TRUE);
}

///////////////////////////////////////////////////////////////////////////
void CImpl::UnmuteAll()
{
	MuteAllObjects(CRI_FALSE);
}

///////////////////////////////////////////////////////////////////////////
void CImpl::PauseAll()
{
	PauseAllObjects(CRI_TRUE);
}

///////////////////////////////////////////////////////////////////////////
void CImpl::ResumeAll()
{
	PauseAllObjects(CRI_FALSE);
}

///////////////////////////////////////////////////////////////////////////
ERequestStatus CImpl::StopAllSounds()
{
	criAtomExPlayer_StopAllPlayers();
	return ERequestStatus::Success;
}

//////////////////////////////////////////////////////////////////////////
void CImpl::SetGlobalParameter(IParameterConnection* const pIParameterConnection, float const value)
{
	if (pIParameterConnection != nullptr)
	{
		auto const pParameter = static_cast<CParameter*>(pIParameterConnection);
		EParameterType const type = pParameter->GetType();

		switch (type)
		{
		case EParameterType::AisacControl:
			{
				for (auto const pObject : g_constructedObjects)
				{
					pParameter->Set(pObject, value);
				}

				break;
			}
		case EParameterType::Category:
			{
				criAtomExCategory_SetVolumeByName(pParameter->GetName(), static_cast<CriFloat32>(pParameter->GetMultiplier() * value + pParameter->GetValueShift()));

				break;
			}
		case EParameterType::GameVariable:
			{
				criAtomEx_SetGameVariableByName(pParameter->GetName(), static_cast<CriFloat32>(pParameter->GetMultiplier() * value + pParameter->GetValueShift()));

				break;
			}
		default:
			{
				Cry::Audio::Log(ELogType::Warning, "Adx2 - Unknown EParameterType: %" PRISIZE_T, type);

				break;
			}
		}
	}
	else
	{
		Cry::Audio::Log(ELogType::Error, "Adx2 - Invalid Parameter pointer passed to the Adx2 implementation of %s", __FUNCTION__);
	}
}

//////////////////////////////////////////////////////////////////////////
void CImpl::SetGlobalSwitchState(ISwitchStateConnection* const pISwitchStateConnection)
{
	if (pISwitchStateConnection != nullptr)
	{
		auto const pSwitchState = static_cast<CSwitchState*>(pISwitchStateConnection);
		ESwitchType const type = pSwitchState->GetType();

		switch (type)
		{
		case ESwitchType::Selector:
		case ESwitchType::AisacControl:
			{
				for (auto const pObject : g_constructedObjects)
				{
					pSwitchState->Set(pObject);
				}

				break;
			}
		case ESwitchType::Category:
			{
				criAtomExCategory_SetVolumeByName(pSwitchState->GetName(), pSwitchState->GetValue());

				break;
			}
		case ESwitchType::GameVariable:
			{
				criAtomEx_SetGameVariableByName(pSwitchState->GetName(), pSwitchState->GetValue());

				break;
			}
		default:
			{
				Cry::Audio::Log(ELogType::Warning, "Adx2 - Unknown ESwitchType: %" PRISIZE_T, type);

				break;
			}
		}
	}
	else
	{
		Cry::Audio::Log(ELogType::Error, "Invalid switch pointer passed to the Adx2 implementation of %s", __FUNCTION__);
	}
}

//////////////////////////////////////////////////////////////////////////
void CImpl::RegisterInMemoryFile(SFileInfo* const pFileInfo)
{
	if (pFileInfo != nullptr)
	{
		auto const pFileData = static_cast<CFile*>(pFileInfo->pImplData);

		if (pFileData != nullptr)
		{
			string acbToAwbPath = pFileInfo->szFilePath;
			PathUtil::RemoveExtension(acbToAwbPath);
			acbToAwbPath += ".awb";

			CriChar8 const* const awbPath = (gEnv->pCryPak->IsFileExist(acbToAwbPath.c_str())) ? static_cast<CriChar8 const*>(acbToAwbPath.c_str()) : nullptr;

			pFileData->pAcb = criAtomExAcb_LoadAcbData(pFileInfo->pFileData, static_cast<CriSint32>(pFileInfo->size), nullptr, awbPath, nullptr, 0);

			if (pFileData->pAcb != nullptr)
			{
				string name = pFileInfo->szFileName;
				PathUtil::RemoveExtension(name);
				g_acbHandles[StringToId(name.c_str())] = pFileData->pAcb;
			}
			else
			{
				Cry::Audio::Log(ELogType::Error, "Failed to load ACB %s\n", pFileInfo->szFileName);
			}
		}
		else
		{
			Cry::Audio::Log(ELogType::Error, "Invalid FileData passed to the Adx2 implementation of %s", __FUNCTION__);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
void CImpl::UnregisterInMemoryFile(SFileInfo* const pFileInfo)
{
	if (pFileInfo != nullptr)
	{
		auto const pFileData = static_cast<CFile*>(pFileInfo->pImplData);

		if ((pFileData != nullptr) && (pFileData->pAcb != nullptr))
		{
			criAtomExAcb_Release(pFileData->pAcb);
		}
		else
		{
			Cry::Audio::Log(ELogType::Error, "Invalid FileData passed to the Adx2 implementation of %s", __FUNCTION__);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
ERequestStatus CImpl::ConstructFile(XmlNodeRef const pRootNode, SFileInfo* const pFileInfo)
{
	ERequestStatus result = ERequestStatus::Failure;

	if ((_stricmp(pRootNode->getTag(), s_szFileTag) == 0) && (pFileInfo != nullptr))
	{
		char const* const szFileName = pRootNode->getAttr(s_szNameAttribute);

		if (szFileName != nullptr && szFileName[0] != '\0')
		{
			char const* const szLocalized = pRootNode->getAttr(s_szLocalizedAttribute);
			pFileInfo->bLocalized = (szLocalized != nullptr) && (_stricmp(szLocalized, s_szTrueValue) == 0);
			pFileInfo->szFileName = szFileName;

			// The Atom library accesses on-memory data with a 32-bit width.
			// The first address of the data must be aligned at a 4-byte boundary.
			pFileInfo->memoryBlockAlignment = 32;

			MEMSTAT_CONTEXT(EMemStatContextTypes::MSC_AudioImpl, 0, "CryAudio::Impl::Adx2::CFile");
			pFileInfo->pImplData = new CFile();

			result = ERequestStatus::Success;
		}
		else
		{
			pFileInfo->szFileName = nullptr;
			pFileInfo->memoryBlockAlignment = 0;
			pFileInfo->pImplData = nullptr;
		}
	}

	return result;
}

//////////////////////////////////////////////////////////////////////////
void CImpl::DestructFile(IFile* const pIFile)
{
	delete pIFile;
}

//////////////////////////////////////////////////////////////////////////
char const* const CImpl::GetFileLocation(SFileInfo* const pFileInfo)
{
	char const* szResult = nullptr;

	if (pFileInfo != nullptr)
	{
		szResult = pFileInfo->bLocalized ? m_localizedSoundBankFolder.c_str() : m_regularSoundBankFolder.c_str();
	}

	return szResult;
}

//////////////////////////////////////////////////////////////////////////
void CImpl::GetInfo(SImplInfo& implInfo) const
{
#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
	implInfo.name = m_name.c_str();
#else
	implInfo.name = "name-not-present-in-release-mode";
#endif  // INCLUDE_ADX2_IMPL_PRODUCTION_CODE
	implInfo.folderName = s_szImplFolderName;
}

///////////////////////////////////////////////////////////////////////////
IObject* CImpl::ConstructGlobalObject()
{
	MEMSTAT_CONTEXT(EMemStatContextTypes::MSC_AudioImpl, 0, "CryAudio::Impl::Adx2::CGlobalObject");
	new CGlobalObject;

	if (!stl::push_back_unique(g_constructedObjects, static_cast<CBaseObject*>(g_pObject)))
	{
		Cry::Audio::Log(ELogType::Warning, "Trying to construct an already registered object.");
	}

	return static_cast<IObject*>(g_pObject);
}

///////////////////////////////////////////////////////////////////////////
IObject* CImpl::ConstructObject(CTransformation const& transformation, char const* const szName /*= nullptr*/)
{
	MEMSTAT_CONTEXT(EMemStatContextTypes::MSC_AudioImpl, 0, "CryAudio::Impl::Adx2::CObject");
	auto const pObject = new CObject(transformation);

#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
	if (szName != nullptr)
	{
		pObject->SetName(szName);
	}
#endif  // INCLUDE_ADX2_IMPL_PRODUCTION_CODE

	if (!stl::push_back_unique(g_constructedObjects, pObject))
	{
		Cry::Audio::Log(ELogType::Warning, "Trying to construct an already registered object.");
	}

	return static_cast<IObject*>(pObject);
}

///////////////////////////////////////////////////////////////////////////
void CImpl::DestructObject(IObject const* const pIObject)
{
	auto const pObject = static_cast<CBaseObject const*>(pIObject);

	if (!stl::find_and_erase(g_constructedObjects, pObject))
	{
		Cry::Audio::Log(ELogType::Warning, "Trying to delete a non-existing object.");
	}

	delete pObject;
}

///////////////////////////////////////////////////////////////////////////
IListener* CImpl::ConstructListener(CTransformation const& transformation, char const* const szName /*= nullptr*/)
{
	IListener* pIListener = nullptr;

	static uint16 id = 0;
	CriAtomEx3dListenerHn const pHandle = criAtomEx3dListener_Create(&m_listenerConfig, nullptr, 0);

	if (pHandle != nullptr)
	{
		MEMSTAT_CONTEXT(EMemStatContextTypes::MSC_AudioImpl, 0, "CryAudio::Impl::Adx2::CListener");
		g_pListener = new CListener(transformation, id++, pHandle);

#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
		if (szName != nullptr)
		{
			g_pListener->SetName(szName);
		}
#endif    // INCLUDE_ADX2_IMPL_PRODUCTION_CODE

		pIListener = static_cast<IListener*>(g_pListener);
	}

	return pIListener;
}

///////////////////////////////////////////////////////////////////////////
void CImpl::DestructListener(IListener* const pIListener)
{
	CRY_ASSERT_MESSAGE(pIListener == g_pListener, "pIListener is not g_pListener during %s", __FUNCTION__);
	criAtomEx3dListener_Destroy(g_pListener->GetHandle());
	delete g_pListener;
	g_pListener = nullptr;
}

//////////////////////////////////////////////////////////////////////////
IStandaloneFileConnection* CImpl::ConstructStandaloneFileConnection(CryAudio::CStandaloneFile& standaloneFile, char const* const szFile, bool const bLocalized, ITriggerConnection const* pITriggerConnection /*= nullptr*/)
{
	MEMSTAT_CONTEXT(EMemStatContextTypes::MSC_AudioImpl, 0, "CryAudio::Impl::Adx2::CStandaloneFile");
	return static_cast<IStandaloneFileConnection*>(new CStandaloneFile);
}

//////////////////////////////////////////////////////////////////////////
void CImpl::DestructStandaloneFileConnection(IStandaloneFileConnection const* const pIStandaloneFileConnection)
{
	CRY_ASSERT_MESSAGE(pIStandaloneFileConnection != nullptr, "pIStandaloneFileConnection is nullptr during %s", __FUNCTION__);
	delete pIStandaloneFileConnection;
}

//////////////////////////////////////////////////////////////////////////
void CImpl::GamepadConnected(DeviceId const deviceUniqueID)
{
}

//////////////////////////////////////////////////////////////////////////
void CImpl::GamepadDisconnected(DeviceId const deviceUniqueID)
{
}

///////////////////////////////////////////////////////////////////////////
ITriggerConnection* CImpl::ConstructTriggerConnection(XmlNodeRef const pRootNode, float& radius)
{
	ITriggerConnection* pITriggerConnection = nullptr;

	if (_stricmp(pRootNode->getTag(), s_szCueTag) == 0)
	{
		char const* const szName = pRootNode->getAttr(s_szNameAttribute);
		char const* const szCueSheetName = pRootNode->getAttr(s_szCueSheetAttribute);

		EActionType actionType = EActionType::Start;
		char const* const szEventType = pRootNode->getAttr(s_szTypeAttribute);

		if ((szEventType != nullptr) && (szEventType[0] != '\0'))
		{
			if (_stricmp(szEventType, s_szStopValue) == 0)
			{
				actionType = EActionType::Stop;
			}
			else if (_stricmp(szEventType, s_szPauseValue) == 0)
			{
				actionType = EActionType::Pause;
			}
			else if (_stricmp(szEventType, s_szResumeValue) == 0)
			{
				actionType = EActionType::Resume;
			}
		}

#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
		if (actionType == EActionType::Start)
		{
			auto const iter = g_cueRadiusInfo.find(szName);

			if (iter != g_cueRadiusInfo.end())
			{
				for (auto const& pair : iter->second)
				{
					if (_stricmp(szCueSheetName, pair.first) == 0)
					{
						radius = pair.second;
						break;
					}
				}
			}
		}

		MEMSTAT_CONTEXT(EMemStatContextTypes::MSC_AudioImpl, 0, "CryAudio::Impl::Adx2::CTrigger");
		pITriggerConnection = static_cast<ITriggerConnection*>(new CTrigger(StringToId(szName), szName, StringToId(szCueSheetName), ETriggerType::Trigger, actionType, szCueSheetName));
#else
		MEMSTAT_CONTEXT(EMemStatContextTypes::MSC_AudioImpl, 0, "CryAudio::Impl::Adx2::CTrigger");
		pITriggerConnection = static_cast<ITriggerConnection*>(new CTrigger(StringToId(szName), szName, StringToId(szCueSheetName), ETriggerType::Trigger, actionType));
#endif    // INCLUDE_ADX2_IMPL_PRODUCTION_CODE
	}
	else if (_stricmp(pRootNode->getTag(), s_szSnapshotTag) == 0)
	{
		char const* const szName = pRootNode->getAttr(s_szNameAttribute);
		int changeoverTime = s_defaultChangeoverTime;
		pRootNode->getAttr(s_szTimeAttribute, changeoverTime);

		MEMSTAT_CONTEXT(EMemStatContextTypes::MSC_AudioImpl, 0, "CryAudio::Impl::Adx2::CTrigger");
#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
		pITriggerConnection = static_cast<ITriggerConnection*>(new CTrigger(StringToId(szName), szName, 0, ETriggerType::Snapshot, EActionType::Start, "", static_cast<CriSint32>(changeoverTime)));
#else
		pITriggerConnection = static_cast<ITriggerConnection*>(new CTrigger(StringToId(szName), szName, 0, ETriggerType::Snapshot, EActionType::Start, static_cast<CriSint32>(changeoverTime)));
#endif    // INCLUDE_ADX2_IMPL_PRODUCTION_CODE
	}
	else
	{
		Cry::Audio::Log(ELogType::Warning, "Unknown Adx2 tag: %s", pRootNode->getTag());
	}

	return pITriggerConnection;
}

//////////////////////////////////////////////////////////////////////////
ITriggerConnection* CImpl::ConstructTriggerConnection(ITriggerInfo const* const pITriggerInfo)
{
#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
	ITriggerConnection* pITriggerConnection = nullptr;
	auto const pTriggerInfo = static_cast<STriggerInfo const*>(pITriggerInfo);

	if (pTriggerInfo != nullptr)
	{
		char const* const szName = pTriggerInfo->name.c_str();
		char const* const szCueSheetName = pTriggerInfo->cueSheet.c_str();

		MEMSTAT_CONTEXT(EMemStatContextTypes::MSC_AudioImpl, 0, "CryAudio::Impl::Adx2::CTrigger");
		pITriggerConnection = static_cast<ITriggerConnection*>(new CTrigger(StringToId(szName), szName, StringToId(szCueSheetName), ETriggerType::Trigger, EActionType::Start, szCueSheetName));
	}

	return pITriggerConnection;
#else
	return nullptr;
#endif  // INCLUDE_ADX2_IMPL_PRODUCTION_CODE
}

///////////////////////////////////////////////////////////////////////////
void CImpl::DestructTriggerConnection(ITriggerConnection const* const pITriggerConnection)
{
	delete pITriggerConnection;
}

///////////////////////////////////////////////////////////////////////////
IParameterConnection* CImpl::ConstructParameterConnection(XmlNodeRef const pRootNode)
{
	IParameterConnection* pIParameterConnection = nullptr;

	char const* const szTag = pRootNode->getTag();
	EParameterType type = EParameterType::None;

	if (_stricmp(szTag, s_szAisacControlTag) == 0)
	{
		type = EParameterType::AisacControl;
	}
	else if (_stricmp(szTag, s_szSCategoryTag) == 0)
	{
		type = EParameterType::Category;
	}
	else if (_stricmp(szTag, s_szGameVariableTag) == 0)
	{
		type = EParameterType::GameVariable;
	}

	if (type != EParameterType::None)
	{
		char const* const szName = pRootNode->getAttr(s_szNameAttribute);
		float multiplier = s_defaultParamMultiplier;
		float shift = s_defaultParamShift;
		pRootNode->getAttr(s_szMutiplierAttribute, multiplier);
		pRootNode->getAttr(s_szShiftAttribute, shift);

		MEMSTAT_CONTEXT(EMemStatContextTypes::MSC_AudioImpl, 0, "CryAudio::Impl::Adx2::CParameter");
		pIParameterConnection = static_cast<IParameterConnection*>(new CParameter(szName, type, multiplier, shift));
	}
	else
	{
		Cry::Audio::Log(ELogType::Warning, "Unknown Adx2 tag: %s", szTag);
	}

	return pIParameterConnection;
}

///////////////////////////////////////////////////////////////////////////
void CImpl::DestructParameterConnection(IParameterConnection const* const pIParameterConnection)
{
	delete pIParameterConnection;
}

//////////////////////////////////////////////////////////////////////////
bool ParseSwitchOrState(XmlNodeRef const pNode, string& selectorName, string& selectorLabelName)
{
	bool foundAttributes = false;

	char const* const szSelectorName = pNode->getAttr(s_szNameAttribute);

	if ((szSelectorName != nullptr) && (szSelectorName[0] != 0) && (pNode->getChildCount() == 1))
	{
		XmlNodeRef const pLabelNode(pNode->getChild(0));

		if (pLabelNode && _stricmp(pLabelNode->getTag(), s_szSelectorLabelTag) == 0)
		{
			char const* const szSelctorLabelName = pLabelNode->getAttr(s_szNameAttribute);

			if ((szSelctorLabelName != nullptr) && (szSelctorLabelName[0] != 0))
			{
				selectorName = szSelectorName;
				selectorLabelName = szSelctorLabelName;
				foundAttributes = true;
			}
		}
	}
	else
	{
		Cry::Audio::Log(ELogType::Warning, "An Adx2 Selector %s inside SwitchState needs to have exactly one Selector Label.", szSelectorName);
	}

	return foundAttributes;
}

///////////////////////////////////////////////////////////////////////////
ISwitchStateConnection* CImpl::ConstructSwitchStateConnection(XmlNodeRef const pRootNode)
{
	ISwitchStateConnection* pISwitchStateConnection = nullptr;

	char const* const szTag = pRootNode->getTag();
	ESwitchType type = ESwitchType::None;

	if (_stricmp(szTag, s_szSelectorTag) == 0)
	{
		type = ESwitchType::Selector;
	}
	else if (_stricmp(szTag, s_szAisacControlTag) == 0)
	{
		type = ESwitchType::AisacControl;
	}
	else if (_stricmp(szTag, s_szGameVariableTag) == 0)
	{
		type = ESwitchType::GameVariable;
	}
	else if (_stricmp(szTag, s_szSCategoryTag) == 0)
	{
		type = ESwitchType::Category;
	}

	if (type == ESwitchType::Selector)
	{
		string szSelectorName;
		string szSelectorLabelName;

		if (ParseSwitchOrState(pRootNode, szSelectorName, szSelectorLabelName))
		{
			MEMSTAT_CONTEXT(EMemStatContextTypes::MSC_AudioImpl, 0, "CryAudio::Impl::Adx2::CSwitchState");
			pISwitchStateConnection = static_cast<ISwitchStateConnection*>(new CSwitchState(type, szSelectorName, szSelectorLabelName));
		}
	}
	else if (type != ESwitchType::None)
	{
		char const* const szName = pRootNode->getAttr(s_szNameAttribute);
		float value = (type == ESwitchType::Category) ? s_defaultCategoryVolume : s_defaultStateValue;

		if (pRootNode->getAttr(s_szValueAttribute, value))
		{
			MEMSTAT_CONTEXT(EMemStatContextTypes::MSC_AudioImpl, 0, "CryAudio::Impl::Adx2::CSwitchState");
			pISwitchStateConnection = static_cast<ISwitchStateConnection*>(new CSwitchState(type, szName, "", static_cast<CriFloat32>(value)));
		}
	}
	else
	{
		Cry::Audio::Log(ELogType::Warning, "Unknown Adx2 tag: %s", szTag);
	}

	return pISwitchStateConnection;
}

///////////////////////////////////////////////////////////////////////////
void CImpl::DestructSwitchStateConnection(ISwitchStateConnection const* const pISwitchStateConnection)
{
	delete pISwitchStateConnection;
}

///////////////////////////////////////////////////////////////////////////
IEnvironmentConnection* CImpl::ConstructEnvironmentConnection(XmlNodeRef const pRootNode)
{
	IEnvironmentConnection* pEnvironmentConnection = nullptr;

	char const* const szTag = pRootNode->getTag();

	if (_stricmp(szTag, s_szBusTag) == 0)
	{
		char const* const szName = pRootNode->getAttr(s_szNameAttribute);

		MEMSTAT_CONTEXT(EMemStatContextTypes::MSC_AudioImpl, 0, "CryAudio::Impl::Adx2::CEnvironment");
		pEnvironmentConnection = static_cast<IEnvironmentConnection*>(new CEnvironment(szName, EEnvironmentType::Bus));
	}
	else if (_stricmp(szTag, s_szAisacControlTag) == 0)
	{
		char const* const szName = pRootNode->getAttr(s_szNameAttribute);
		float multiplier = s_defaultParamMultiplier;
		float shift = s_defaultParamShift;
		pRootNode->getAttr(s_szMutiplierAttribute, multiplier);
		pRootNode->getAttr(s_szShiftAttribute, shift);

		MEMSTAT_CONTEXT(EMemStatContextTypes::MSC_AudioImpl, 0, "CryAudio::Impl::Adx2::CEnvironment");
		pEnvironmentConnection = static_cast<IEnvironmentConnection*>(new CEnvironment(szName, EEnvironmentType::AisacControl, multiplier, shift));
	}
	else
	{
		Cry::Audio::Log(ELogType::Warning, "Unknown Adx2 tag: %s", szTag);
	}

	return pEnvironmentConnection;
}

///////////////////////////////////////////////////////////////////////////
void CImpl::DestructEnvironmentConnection(IEnvironmentConnection const* const pIEnvironmentConnection)
{
	delete pIEnvironmentConnection;
}

//////////////////////////////////////////////////////////////////////////
ISettingConnection* CImpl::ConstructSettingConnection(XmlNodeRef const pRootNode)
{
	ISettingConnection* pISettingConnection = nullptr;

	char const* const szTag = pRootNode->getTag();

	if (_stricmp(szTag, s_szDspBusSettingTag) == 0)
	{
		char const* const szName = pRootNode->getAttr(s_szNameAttribute);

		MEMSTAT_CONTEXT(EMemStatContextTypes::MSC_AudioImpl, 0, "CryAudio::Impl::Adx2::CSetting");
		pISettingConnection = static_cast<ISettingConnection*>(new CSetting(szName));
	}
	else
	{
		Cry::Audio::Log(ELogType::Warning, "Unknown Adx2 tag: %s", szTag);
	}

	return pISettingConnection;
}

//////////////////////////////////////////////////////////////////////////
void CImpl::DestructSettingConnection(ISettingConnection const* const pISettingConnection)
{
	delete pISettingConnection;
}

//////////////////////////////////////////////////////////////////////////
void CImpl::OnRefresh()
{
#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
	g_debugCurrentDspBusSettingName = g_debugNoneDspBusSetting;
	m_acfFileSize = 0;
	g_cueRadiusInfo.clear();
	LoadAcbInfos(m_regularSoundBankFolder);
	LoadAcbInfos(m_localizedSoundBankFolder);
#endif  // INCLUDE_ADX2_IMPL_PRODUCTION_CODE

	criAtomEx_UnregisterAcf();
	RegisterAcf();
	g_acbHandles.clear();
}

//////////////////////////////////////////////////////////////////////////
void CImpl::SetLanguage(char const* const szLanguage)
{
	if (szLanguage != nullptr)
	{
		m_language = szLanguage;
		m_localizedSoundBankFolder = PathUtil::GetLocalizationFolder().c_str();
		m_localizedSoundBankFolder += "/";
		m_localizedSoundBankFolder += m_language.c_str();
		m_localizedSoundBankFolder += "/";
		m_localizedSoundBankFolder += AUDIO_SYSTEM_DATA_ROOT;
		m_localizedSoundBankFolder += "/";
		m_localizedSoundBankFolder += s_szImplFolderName;
		m_localizedSoundBankFolder += "/";
		m_localizedSoundBankFolder += s_szAssetsFolderName;
	}
}

//////////////////////////////////////////////////////////////////////////
CEvent* CImpl::ConstructEvent(TriggerInstanceId const triggerInstanceId)
{
	MEMSTAT_CONTEXT(EMemStatContextTypes::MSC_AudioImpl, 0, "CryAudio::Impl::Adx2::CEvent");
	auto const pEvent = new CEvent(triggerInstanceId);

#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
	g_constructedEvents.push_back(pEvent);
#endif  // INCLUDE_ADX2_IMPL_PRODUCTION_CODE

	return pEvent;
}

//////////////////////////////////////////////////////////////////////////
void CImpl::DestructEvent(CEvent const* const pEvent)
{
#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
	CRY_ASSERT_MESSAGE(pEvent != nullptr, "pEvent is nullpter during %s", __FUNCTION__);

	auto iter(g_constructedEvents.begin());
	auto const iterEnd(g_constructedEvents.cend());

	for (; iter != iterEnd; ++iter)
	{
		if ((*iter) == pEvent)
		{
			if (iter != (iterEnd - 1))
			{
				(*iter) = g_constructedEvents.back();
			}

			g_constructedEvents.pop_back();
			break;
		}
	}
#endif  // INCLUDE_ADX2_IMPL_PRODUCTION_CODE

	delete pEvent;
}

//////////////////////////////////////////////////////////////////////////
void CImpl::GetFileData(char const* const szName, SFileData& fileData) const
{
}

//////////////////////////////////////////////////////////////////////////
bool CImpl::InitializeLibrary()
{
#if defined(CRY_PLATFORM_WINDOWS)
	CriAtomExConfig_WASAPI libraryConfig;
	criAtomEx_SetDefaultConfig_WASAPI(&libraryConfig);
#else
	CriAtomExConfig libraryConfig;
	criAtomEx_SetDefaultConfig(&libraryConfig);
#endif  // CRY_PLATFORM_WINDOWS

	libraryConfig.atom_ex.max_virtual_voices = static_cast<CriSint32>(g_cvars.m_maxVirtualVoices);
	libraryConfig.atom_ex.max_voice_limit_groups = static_cast<CriSint32>(g_cvars.m_maxVoiceLimitGroups);
	libraryConfig.atom_ex.max_categories = static_cast<CriSint32>(g_cvars.m_maxCategories);
	libraryConfig.atom_ex.categories_per_playback = static_cast<CriSint32>(g_cvars.m_categoriesPerPlayback);
	libraryConfig.atom_ex.max_tracks = static_cast<CriUint32>(g_cvars.m_maxTracks);
	libraryConfig.atom_ex.max_track_items = static_cast<CriUint32>(g_cvars.m_maxTrackItems);
	libraryConfig.atom_ex.max_faders = static_cast<CriUint32>(g_cvars.m_maxFaders);
	libraryConfig.atom_ex.max_pitch = static_cast<CriFloat32>(g_cvars.m_maxPitch);
	libraryConfig.asr.num_buses = static_cast<CriSint32>(g_cvars.m_numBuses);
	libraryConfig.asr.output_channels = static_cast<CriSint32>(g_cvars.m_outputChannels);
	libraryConfig.asr.output_sampling_rate = static_cast<CriSint32>(g_cvars.m_outputSamplingRate);

#if defined(CRY_PLATFORM_WINDOWS)
	criAtomEx_Initialize_WASAPI(&libraryConfig, nullptr, 0);
#else
	criAtomEx_Initialize(&libraryConfig, nullptr, 0);
#endif  // CRY_PLATFORM_WINDOWS

	bool const isInitialized = (criAtomEx_IsInitialized() == CRI_TRUE);

	if (!isInitialized)
	{
		Cry::Audio::Log(ELogType::Error, "Failed to initialize CriAtomEx");
	}

	return isInitialized;
}

//////////////////////////////////////////////////////////////////////////
bool CImpl::AllocateVoicePool()
{
	CriAtomExStandardVoicePoolConfig voicePoolConfig;
	criAtomExVoicePool_SetDefaultConfigForStandardVoicePool(&voicePoolConfig);
	voicePoolConfig.num_voices = static_cast<CriSint32>(g_cvars.m_numVoices);
	voicePoolConfig.player_config.max_channels = static_cast<CriSint32>(g_cvars.m_maxChannels);
	voicePoolConfig.player_config.max_sampling_rate = static_cast<CriSint32>(g_cvars.m_maxSamplingRate);
	voicePoolConfig.player_config.streaming_flag = CRI_TRUE;

	bool const isAllocated = (criAtomExVoicePool_AllocateStandardVoicePool(&voicePoolConfig, nullptr, 0) != nullptr);

	if (!isAllocated)
	{
		Cry::Audio::Log(ELogType::Error, "Failed to allocate voice pool");
	}

	return isAllocated;
}

//////////////////////////////////////////////////////////////////////////
bool CImpl::CreateDbas()
{
	CriAtomDbasConfig dbasConfig;
	criAtomDbas_SetDefaultConfig(&dbasConfig);
	dbasConfig.max_streams = static_cast<CriSint32>(g_cvars.m_maxStreams);
	m_dbasId = criAtomExDbas_Create(&dbasConfig, nullptr, 0);

	bool const isCreated = (m_dbasId != CRIATOMEXDBAS_ILLEGAL_ID);

	if (!isCreated)
	{
		Cry::Audio::Log(ELogType::Error, "Failed to create D-BAS");
	}

	return isCreated;
}

//////////////////////////////////////////////////////////////////////////
bool CImpl::RegisterAcf()
{
	bool acfRegistered = false;
	bool acfExists = false;

	CryFixedStringT<MaxFilePathLength> acfPath;
	CryFixedStringT<MaxFilePathLength> search(m_regularSoundBankFolder + "/*.acf");

	_finddata_t fd;
	intptr_t const handle = gEnv->pCryPak->FindFirst(search.c_str(), &fd);

	if (handle != -1)
	{
		do
		{
			char const* const fileName = fd.name;

			if (_stricmp(PathUtil::GetExt(fileName), "acf") == 0)
			{
				acfPath = m_regularSoundBankFolder + "/" + fileName;
				acfExists = true;
				break;
			}
		}
		while (gEnv->pCryPak->FindNext(handle, &fd) >= 0);

		gEnv->pCryPak->FindClose(handle);
	}

	if (acfExists)
	{
		FILE* const pAcfFile = gEnv->pCryPak->FOpen(acfPath.c_str(), "rbx");
		size_t const acfFileSize = gEnv->pCryPak->FGetSize(acfPath.c_str());

		MEMSTAT_CONTEXT(EMemStatContextTypes::MSC_AudioImpl, 0, "CryAudio::Impl::Adx2::CImpl::m_pAcfBuffer");
		m_pAcfBuffer = new uint8[acfFileSize];
		gEnv->pCryPak->FRead(m_pAcfBuffer, acfFileSize, pAcfFile);
		gEnv->pCryPak->FClose(pAcfFile);

		auto const pAcfData = static_cast<void*>(m_pAcfBuffer);

		criAtomEx_RegisterAcfData(pAcfData, static_cast<CriSint32>(acfFileSize), nullptr, 0);
		CriAtomExAcfInfo acfInfo;

		if (criAtomExAcf_GetAcfInfo(&acfInfo) == CRI_TRUE)
		{
			acfRegistered = true;

#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
			m_acfFileSize = acfFileSize;
#endif      // INCLUDE_ADX2_IMPL_PRODUCTION_CODE
		}
		else
		{
			Cry::Audio::Log(ELogType::Error, "Failed to register ACF");
		}
	}
	else
	{
		Cry::Audio::Log(ELogType::Error, "ACF not found.");
	}

	return acfRegistered;
}

//////////////////////////////////////////////////////////////////////////
void CImpl::InitializeFileSystem()
{
	criFs_SetUserMallocFunction(userMalloc, nullptr);
	criFs_SetUserFreeFunction(userFree, nullptr);

	CriFsConfig fileSystemConfig;
	criFs_SetDefaultConfig(&fileSystemConfig);
	fileSystemConfig.max_files = static_cast<CriSint32>(g_cvars.m_maxFiles);

	criFs_InitializeLibrary(&fileSystemConfig, nullptr, 0);
	criFs_SetSelectIoCallback(selectIoFunc);
}

//////////////////////////////////////////////////////////////////////////
void CImpl::SetListenerConfig()
{
	criAtomEx3dListener_SetDefaultConfig(&m_listenerConfig);
}

//////////////////////////////////////////////////////////////////////////
void CImpl::SetPlayerConfig()
{
	criAtomExPlayer_SetDefaultConfig(&g_playerConfig);
	g_playerConfig.voice_allocation_method = static_cast<CriAtomExVoiceAllocationMethod>(g_cvars.m_voiceAllocationMethod);
}

//////////////////////////////////////////////////////////////////////////
void CImpl::Set3dSourceConfig()
{
	g_3dSourceConfig.enable_voice_priority_decay = CRI_TRUE;
}

//////////////////////////////////////////////////////////////////////////
void CImpl::MuteAllObjects(CriBool const shouldMute)
{
	for (auto const pObject : g_constructedObjects)
	{
		pObject->MutePlayer(shouldMute);
	}
}

//////////////////////////////////////////////////////////////////////////
void CImpl::PauseAllObjects(CriBool const shouldPause)
{
	for (auto const pObject : g_constructedObjects)
	{
		pObject->PausePlayer(shouldPause);
	}
}

#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
//////////////////////////////////////////////////////////////////////////
void DrawMemoryPoolInfo(
	IRenderAuxGeom& auxGeom,
	float const posX,
	float& posY,
	stl::SPoolMemoryUsage const& mem,
	stl::SMemoryUsage const& pool,
	char const* const szType,
	uint16 const poolSize)
{
	CryFixedStringT<MaxMiscStringLength> memUsedString;

	if (mem.nUsed < 1024)
	{
		memUsedString.Format("%" PRISIZE_T " Byte", mem.nUsed);
	}
	else
	{
		memUsedString.Format("%" PRISIZE_T " KiB", mem.nUsed >> 10);
	}

	CryFixedStringT<MaxMiscStringLength> memAllocString;

	if (mem.nAlloc < 1024)
	{
		memAllocString.Format("%" PRISIZE_T " Byte", mem.nAlloc);
	}
	else
	{
		memAllocString.Format("%" PRISIZE_T " KiB", mem.nAlloc >> 10);
	}

	ColorF const color = (static_cast<uint16>(pool.nUsed) > poolSize) ? Debug::s_globalColorError : Debug::s_systemColorTextPrimary;

	posY += Debug::s_systemLineHeight;
	auxGeom.Draw2dLabel(posX, posY, Debug::s_systemFontSize, color, false,
	                    "[%s] Constructed: %" PRISIZE_T " (%s) | Allocated: %" PRISIZE_T " (%s) | Pool Size: %u",
	                    szType, pool.nUsed, memUsedString.c_str(), pool.nAlloc, memAllocString.c_str(), poolSize);
}
#endif  // INCLUDE_ADX2_IMPL_PRODUCTION_CODE

//////////////////////////////////////////////////////////////////////////
void CImpl::DrawDebugMemoryInfo(IRenderAuxGeom& auxGeom, float const posX, float& posY, bool const showDetailedInfo)
{
#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
	CryModuleMemoryInfo memInfo;
	ZeroStruct(memInfo);
	CryGetMemoryInfoForModule(&memInfo);

	CryFixedStringT<MaxMiscStringLength> memInfoString;
	auto const memAlloc = static_cast<uint32>(memInfo.allocated - memInfo.freed);

	if (memAlloc < 1024)
	{
		memInfoString.Format("%s (Total Memory: %u Byte)", m_name.c_str(), memAlloc);
	}
	else
	{
		memInfoString.Format("%s (Total Memory: %u KiB)", m_name.c_str(), memAlloc >> 10);
	}

	auxGeom.Draw2dLabel(posX, posY, Debug::s_systemHeaderFontSize, Debug::s_globalColorHeader, false, memInfoString.c_str());
	posY += Debug::s_systemHeaderLineSpacerHeight;

	if (showDetailedInfo)
	{
		{
			auto& allocator = CObject::GetAllocator();
			DrawMemoryPoolInfo(auxGeom, posX, posY, allocator.GetTotalMemory(), allocator.GetCounts(), "Objects", g_objectPoolSize);
		}

		{
			auto& allocator = CEvent::GetAllocator();
			DrawMemoryPoolInfo(auxGeom, posX, posY, allocator.GetTotalMemory(), allocator.GetCounts(), "Cues", g_eventPoolSize);
		}

		if (g_debugPoolSizes.triggers > 0)
		{
			auto& allocator = CTrigger::GetAllocator();
			DrawMemoryPoolInfo(auxGeom, posX, posY, allocator.GetTotalMemory(), allocator.GetCounts(), "Triggers", g_poolSizes.triggers);
		}

		if (g_debugPoolSizes.parameters > 0)
		{
			auto& allocator = CParameter::GetAllocator();
			DrawMemoryPoolInfo(auxGeom, posX, posY, allocator.GetTotalMemory(), allocator.GetCounts(), "Parameters", g_poolSizes.parameters);
		}

		if (g_debugPoolSizes.switchStates > 0)
		{
			auto& allocator = CSwitchState::GetAllocator();
			DrawMemoryPoolInfo(auxGeom, posX, posY, allocator.GetTotalMemory(), allocator.GetCounts(), "SwitchStates", g_poolSizes.switchStates);
		}

		if (g_debugPoolSizes.environments > 0)
		{
			auto& allocator = CEnvironment::GetAllocator();
			DrawMemoryPoolInfo(auxGeom, posX, posY, allocator.GetTotalMemory(), allocator.GetCounts(), "Environments", g_poolSizes.environments);
		}

		if (g_debugPoolSizes.settings > 0)
		{
			auto& allocator = CSetting::GetAllocator();
			DrawMemoryPoolInfo(auxGeom, posX, posY, allocator.GetTotalMemory(), allocator.GetCounts(), "Settings", g_poolSizes.settings);
		}

		if (g_debugPoolSizes.files > 0)
		{
			auto& allocator = CFile::GetAllocator();
			DrawMemoryPoolInfo(auxGeom, posX, posY, allocator.GetTotalMemory(), allocator.GetCounts(), "Files", g_poolSizes.files);
		}
	}

	size_t const numEvents = g_constructedEvents.size();

	posY += Debug::s_systemLineHeight;
	auxGeom.Draw2dLabel(posX, posY, Debug::s_systemFontSize, Debug::s_systemColorTextSecondary, false, "Cues: %3" PRISIZE_T " | Objects with Doppler: %u | DSP Bus Setting: %s",
	                    numEvents, g_numObjectsWithDoppler, g_debugCurrentDspBusSettingName.c_str());

	Vec3 const& listenerPosition = g_pListener->GetPosition();
	Vec3 const& listenerDirection = g_pListener->GetTransformation().GetForward();
	float const listenerVelocity = g_pListener->GetVelocity().GetLength();
	char const* const szName = g_pListener->GetName();

	posY += Debug::s_systemLineHeight;
	auxGeom.Draw2dLabel(posX, posY, Debug::s_systemFontSize, Debug::s_systemColorListenerActive, false, "Listener: %s | PosXYZ: %.2f %.2f %.2f | FwdXYZ: %.2f %.2f %.2f | Velocity: %.2f m/s",
	                    szName, listenerPosition.x, listenerPosition.y, listenerPosition.z, listenerDirection.x, listenerDirection.y, listenerDirection.z, listenerVelocity);
#endif  // INCLUDE_ADX2_IMPL_PRODUCTION_CODE
}

//////////////////////////////////////////////////////////////////////////
void CImpl::DrawDebugInfoList(IRenderAuxGeom& auxGeom, float& posX, float posY, float const debugDistance, char const* const szTextFilter) const
{
#if defined(INCLUDE_ADX2_IMPL_PRODUCTION_CODE)
	CryFixedStringT<MaxControlNameLength> lowerCaseSearchString(szTextFilter);
	lowerCaseSearchString.MakeLower();

	auxGeom.Draw2dLabel(posX, posY, Debug::s_listHeaderFontSize, Debug::s_globalColorHeader, false, "Adx2 Cues [%" PRISIZE_T "]", g_constructedEvents.size());
	posY += Debug::s_listHeaderLineHeight;

	for (auto const pEvent : g_constructedEvents)
	{
		Vec3 const& position = pEvent->GetObject()->GetTransformation().GetPosition();
		float const distance = position.GetDistance(g_pListener->GetPosition());

		if ((debugDistance <= 0.0f) || ((debugDistance > 0.0f) && (distance < debugDistance)))
		{
			char const* const szTriggerName = pEvent->GetName();
			CryFixedStringT<MaxControlNameLength> lowerCaseTriggerName(szTriggerName);
			lowerCaseTriggerName.MakeLower();
			bool const draw = ((lowerCaseSearchString.empty() || (lowerCaseSearchString == "0")) || (lowerCaseTriggerName.find(lowerCaseSearchString) != CryFixedStringT<MaxControlNameLength>::npos));

			if (draw)
			{
				ColorF const color = ((pEvent->GetFlags() & EEventFlags::IsVirtual) != 0) ? Debug::s_globalColorVirtual : Debug::s_listColorItemActive;
				auxGeom.Draw2dLabel(posX, posY, Debug::s_listFontSize, color, false, "%s on %s", szTriggerName, pEvent->GetObject()->GetName());

				posY += Debug::s_listLineHeight;
			}
		}
	}

	posX += 600.0f;
#endif  // INCLUDE_ADX2_IMPL_PRODUCTION_CODE
}
} // namespace Adx2
} // namespace Impl
} // namespace CryAudio
