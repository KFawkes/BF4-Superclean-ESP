#pragma once
#include <Windows.h>
#include <cstdio>
#include <d3d11.h>
#include <thread>
#include <chrono>
#include <array>
#include <map>
#include <Psapi.h>
#include <mutex>

#include "sdk.h"
#include "xorstr.h"
#include "MinHook.h"
#include "utils.h"

using namespace DirectX::SimpleMath;

using DispatchMessage_t = void(*)(void* pMessageManagerImpl, Message* pMessage);
DispatchMessage_t oDispatchMessage = nullptr;

using Present_t = HRESULT(*)(IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags);
Present_t oPresent = nullptr;

using PreFrameUpdate_t = void(*)(uintptr_t pthis, uint64_t a2);
PreFrameUpdate_t oPreFrameUpdate = nullptr;

using CopySubresourceRegion_t = void(*)(
	ID3D11DeviceContext* pContext,
	ID3D11Resource* pDstResource,
	UINT            DstSubresource,
	UINT            DstX,
	UINT            DstY,
	UINT            DstZ,
	ID3D11Resource* pSrcResource,
	UINT            SrcSubresource,
	const D3D11_BOX* pSrcBox);
CopySubresourceRegion_t oCopySubresourceRegion = nullptr;

uint32_t dxWidth;
uint32_t dxHeight;
Matrix m_ViewProj;

#define PLAYERS_SIZE sizeof(PlayersLogicData) * 70
struct PlayersLogicData {
	unsigned char visible;
	float health;
	float maxHealth;
	TransformAABBStruct transform;
	bool vehicle;
	float healthVehicle;
	float maxHealthVehicle;
	TransformAABBStruct transformVehicle;
};
PlayersLogicData* playersBuffer;
uint32_t playersCount;
std::mutex playersMutex;

bool pendingFfSs = false;
bool pendingPbSs = false;
uint32_t cleanFrames = 0;
std::mutex ssMutex;
VeniceNetworkRequestMessage* ffSsMsg;

ID3D11Texture2D* pCleanScreenShot = NULL;
DWORD pbLastCleanFrame = 0;

inline bool WorldToScreen(const Vector3& pos, Vector2& out)
{
	float w = m_ViewProj.m[0][3] * pos.x + m_ViewProj.m[1][3] * pos.y + m_ViewProj.m[2][3] * pos.z + m_ViewProj.m[3][3];
	if (w < 0.19)
		return false;
	float x = m_ViewProj.m[0][0] * pos.x + m_ViewProj.m[1][0] * pos.y + m_ViewProj.m[2][0] * pos.z + m_ViewProj.m[3][0];
	float y = m_ViewProj.m[0][1] * pos.x + m_ViewProj.m[1][1] * pos.y + m_ViewProj.m[2][1] * pos.z + m_ViewProj.m[3][1];

	auto hWidth = static_cast<float>(dxWidth) / 2.0f;
	auto hHeight = static_cast<float>(dxHeight) / 2.0f;

	auto out_x = hWidth + hWidth * x / w;
	auto out_y = hHeight - hHeight * y / w;

	if (out_x >= 0.0f && out_x < dxWidth && out_y >= 0.0f && out_y < dxHeight) {

		out.x = out_x;
		out.y = out_y;
		//out->z = w;

		return true;
	}
	return false;
}

inline bool WorldToScreen(Vector3& pos)
{
	return WorldToScreen(pos, reinterpret_cast<Vector2&>(pos));
}

auto MultiplyMat(const Vector3& vec, const Matrix* mat)
{
	return Vector3(mat->_11 * vec.x + mat->_21 * vec.y + mat->_31 * vec.z,
		mat->_12 * vec.x + mat->_22 * vec.y + mat->_32 * vec.z,
		mat->_13 * vec.x + mat->_23 * vec.y + mat->_33 * vec.z);
}

/*Vector3 GetBone(ClientSoldierEntity* pEnt, UpdatePoseResultData::BONES boneId) {
	pEnt->m_pRagdollComponent;
	auto pRag = pEnt->m_pRagdollComponent;
	Vector3 vOut;
	if (pRag)
		pRag->GetBone(boneId, vOut);

	return vOut;
}*/

void UpdatePlayers(Level* level, ClientPlayer* localPlayer, ClientPlayer** m_ppPlayers) {
	uint32_t localPlayersCount = 0;
	PlayersLogicData* localPlayersBuffer = (PlayersLogicData *)malloc(PLAYERS_SIZE);

	for (int i = 0; i < 70; i++) {
		auto player = m_ppPlayers[i];
		if (!player)
			continue;

		if (player == localPlayer)
			continue;

		if (player->m_TeamId == localPlayer->m_TeamId)
			continue;

		auto soldier = player->GetSoldier();
		if (!soldier) continue;

		if (soldier->m_pHealthComp->m_Health < 0.1f)
			continue;

		TransformAABBStruct transformSoldier;
		soldier->GetAABB(&transformSoldier);

		TransformAABBStruct transformVehicle;
		auto vehicle = player->GetVehicle();
		float healthVehicle = 0.0f, maxHealthVehicle = 0.0f;
		if (vehicle) {
			vehicle->GetAABB(&transformVehicle);
			auto vehicleData = reinterpret_cast<VehicleEntityData*>(vehicle->m_Data);
			healthVehicle = vehicle->m_pHealthComp->m_VehicleHealth;
			maxHealthVehicle = vehicleData->m_FrontHealthZone.m_MaxHealth;
		}

		if (vehicle)
			vehicle->m_pComponents->GetComponentByClassId<ClientSpottingTargetComponent>(378)->activeSpotType =
				!pendingPbSs && !pendingFfSs ? ClientSpottingTargetComponent::SpotType_Active : ClientSpottingTargetComponent::SpotType_None;
		else
			soldier->m_pComponents->GetComponentByClassId<ClientSpottingTargetComponent>(378)->activeSpotType =
				!pendingPbSs && !pendingFfSs ? ClientSpottingTargetComponent::SpotType_Active : ClientSpottingTargetComponent::SpotType_None;

		localPlayersBuffer[localPlayersCount] = {
			soldier->m_Occluded,
			soldier->m_pHealthComp->m_Health,
			soldier->m_pHealthComp->m_MaxHealth,
			transformSoldier,
			vehicle ? true : false,
			healthVehicle,
			maxHealthVehicle,
			transformVehicle
		};

		localPlayersCount++;
	}

	playersMutex.lock();
	playersCount = localPlayersCount;
	memcpy_s(playersBuffer, PLAYERS_SIZE, localPlayersBuffer, PLAYERS_SIZE);
	playersMutex.unlock();

	free(localPlayersBuffer);
}

void SetJetSpeed(ClientSoldierEntity* localSoldier, BorderInputNode* pBorderInputNode) {
	auto pKeyboard = pBorderInputNode->m_pKeyboard;
	static bool lastFrameKeyPress = false;
	if (pKeyboard->m_pDevice->m_Buffer[InputDeviceKeys::IDK_ArrowUp] || pKeyboard->m_pDevice->m_Buffer[InputDeviceKeys::IDK_ArrowDown]) {
		auto velVec = localSoldier->GetVelocity();
		auto velocity = sqrt(pow(velVec->x, 2) + pow(velVec->y, 2) + pow(velVec->z, 2)) * 3.6f;
		if (velocity > 315.0f) {
			pKeyboard->m_pDevice->m_Buffer[InputDeviceKeys::IDK_S] = 1;
			pKeyboard->m_pDevice->m_Buffer[InputDeviceKeys::IDK_LeftShift] = 0;
		}
		else if (velocity < 310.0f) {
			pKeyboard->m_pDevice->m_Buffer[InputDeviceKeys::IDK_S] = 0;
			pKeyboard->m_pDevice->m_Buffer[InputDeviceKeys::IDK_LeftShift] = 1;
		}
		lastFrameKeyPress = true;
	}
	else if (lastFrameKeyPress) {
		pKeyboard->m_pDevice->m_Buffer[InputDeviceKeys::IDK_S] = 0;
		pKeyboard->m_pDevice->m_Buffer[InputDeviceKeys::IDK_LeftShift] = 0;
		lastFrameKeyPress = false;
	}
}

void InitiateFFScreenShot() {
	static auto last = 0;
	if (GetTickCount() - last < 10000)
		return;
	last = GetTickCount();

	char buffer[88];
	auto ssMsg = (VeniceNetworkRequestScreenshotMessage*)&buffer;
	((__int64 (*) (VeniceNetworkRequestScreenshotMessage*))0x1419F4BA0)(ssMsg);
	ssMsg->width = 0x500;
	ssMsg->height = 0x2d0;
	ssMsg->unknown48 = 1;
	ssMsg->unknown4c = 3;
	ssMsg->unknown50 = 0;
	((void (*) (MessageManager*, Message*))OFFSET_DISPATCHMESSAGE)(ClientGameContext::GetInstance()->m_messageManager, ssMsg);
	ssMsg->destruct(0);
}

void UpdatePreFrameData()
{
	const auto context = ClientGameContext::GetInstance();
	if (!context) return;

	auto pBorderInputNode = BorderInputNode::GetInstance();
	if (!pBorderInputNode) return;

	const auto level = context->m_pLevel;
	if (!level) return;

	const auto world = level->m_pGameWorld;
	if (!world) return;

	const auto playerManager = context->m_pPlayerManager;
	if (!playerManager) return;

	const auto localPlayer = playerManager->m_pLocalPlayer;
	if (!localPlayer) return;

	const auto localSoldier = localPlayer->m_pControlledControllable;
	if (!localSoldier) return;
	
	auto localVehicle = localPlayer->GetVehicle();
	if (localVehicle) {
		auto vehicleData = reinterpret_cast<VehicleEntityData*>(localVehicle->m_Data);
		if (vehicleData)
			if (strstr(vehicleData->m_NameSid, xorstr_("F35")) ||
				strstr(vehicleData->m_NameSid, xorstr_("J20")) ||
				strstr(vehicleData->m_NameSid, xorstr_("PAKFA")) ||
				strstr(vehicleData->m_NameSid, xorstr_("A10")) ||
				strstr(vehicleData->m_NameSid, xorstr_("SU39")) ||
				strstr(vehicleData->m_NameSid, xorstr_("Q5")))

				SetJetSpeed(localSoldier, pBorderInputNode);
	}

	UpdatePlayers(level, localPlayer, playerManager->m_ppPlayers);

	// FairFight Screenshots
	ssMutex.lock();
	if (pendingFfSs) {
		if (cleanFrames > 5) {
			oDispatchMessage(context->m_messageManager, ffSsMsg);
			ffSsMsg->destruct(0);
			pendingFfSs = false;
		}
	}
	ssMutex.unlock();
}

bool GetBoxCords(const TransformAABBStruct& TransAABB, Vector2* cords) {
	Vector3 corners[8];
	Vector3 pos = (Vector3)TransAABB.Transform.m[3];
	Vector3 min = Vector3(TransAABB.AABB.m_Min.x, TransAABB.AABB.m_Min.y, TransAABB.AABB.m_Min.z);
	Vector3 max = Vector3(TransAABB.AABB.m_Max.x, TransAABB.AABB.m_Max.y, TransAABB.AABB.m_Max.z);
	corners[2] = pos + MultiplyMat(Vector3(max.x, min.y, min.z), &TransAABB.Transform);
	corners[3] = pos + MultiplyMat(Vector3(max.x, min.y, max.z), &TransAABB.Transform);
	corners[4] = pos + MultiplyMat(Vector3(min.x, min.y, max.z), &TransAABB.Transform);
	corners[5] = pos + MultiplyMat(Vector3(min.x, max.y, max.z), &TransAABB.Transform);
	corners[6] = pos + MultiplyMat(Vector3(min.x, max.y, min.z), &TransAABB.Transform);
	corners[7] = pos + MultiplyMat(Vector3(max.x, max.y, min.z), &TransAABB.Transform);
	min = pos + MultiplyMat(min, &TransAABB.Transform);
	max = pos + MultiplyMat(max, &TransAABB.Transform);
	corners[0] = min;
	corners[1] = max;

	for (auto& v3 : corners) {
		if (!WorldToScreen(v3))
			return false;
	}

	cords[0].x = dxWidth;
	cords[0].y = dxHeight;
	cords[1].x = 0;
	cords[1].y = 0;

	for (auto& v3 : corners) {
		if (v3.x < cords[0].x)
			cords[0].x = v3.x;
		if (v3.y < cords[0].y)
			cords[0].y = v3.y;
		if (v3.x > cords[1].x)
			cords[1].x = v3.x;
		if (v3.y > cords[1].y)
			cords[1].y = v3.y;
	}

	return true;
}

void UpdateCleanFrame(IDXGISwapChain* pSwapChain, ID3D11Device* pDevice, ID3D11DeviceContext* pContext)
{
	auto currentTickCount = GetTickCount();
	if (currentTickCount > pbLastCleanFrame + 5000) {
		if (cleanFrames > 5) {
			ID3D11Texture2D* pBuffer = NULL;
			HRESULT hr = pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pBuffer));
			D3D11_TEXTURE2D_DESC td;
			memset(&td, 0, sizeof(D3D11_TEXTURE2D_DESC));

			if (pCleanScreenShot)
				pCleanScreenShot->Release();

			pBuffer->GetDesc(&td);
			ssMutex.lock();
			pDevice->CreateTexture2D(&td, NULL, &pCleanScreenShot);
			pContext->CopyResource(pCleanScreenShot, pBuffer);
			ssMutex.unlock();
			pBuffer->Release();

			pbLastCleanFrame = currentTickCount;
			pendingPbSs = false;
		}
		else
			pendingPbSs = true;
	}
}

void DrawESP() {
	auto pDebugRenderer = DebugRenderer2::GetInstance();
	if (!pDebugRenderer)
		return;

	uint32_t localPlayersCount = 0;
	PlayersLogicData* localPlayersBuffer = (PlayersLogicData*)malloc(PLAYERS_SIZE);

	playersMutex.lock();
	localPlayersCount = playersCount;
	memcpy_s(localPlayersBuffer, PLAYERS_SIZE, playersBuffer, PLAYERS_SIZE);
	playersMutex.unlock();

	for (int i = 0; i < localPlayersCount; i ++) {
		auto player = localPlayersBuffer[i];
		Vector2 boxCords[2];
		auto transform = player.vehicle ? player.transformVehicle : player.transform;
		auto health = player.vehicle ? player.healthVehicle : player.health;
		auto maxHealth = player.vehicle ? player.maxHealthVehicle : player.maxHealth;
		if (GetBoxCords(transform, &boxCords[0])) {
			pDebugRenderer->drawBox2d(boxCords[0], boxCords[1], 2.0f, !player.visible ? Color32(255, 0, 0, 255) : Color32(255, 200, 35, 255));

			// Health Bars
			auto boxWidth = boxCords[1].x - boxCords[0].x;
			auto healthBarWidth = max(boxWidth, 16.0f);
			auto healthBarHeight = max(boxWidth / 50.0f, 3.0f);

			auto healthBarWidthOffset = max((healthBarWidth - boxWidth) / 2, 0);
			auto healthBarHeightOffset = 5.0f;

			auto healthBarPercWidth = healthBarWidth * (health / maxHealth);

			Color32 healthBarColor(
				BYTE(255 - max(health - maxHealth / 2, 0) * (255 / (maxHealth / 2))),
				BYTE(255 - max(maxHealth / 2 - health, 0) * (255 / (maxHealth / 2))),
				0,
				255);

			pDebugRenderer->drawRect2d(
				boxCords[0].x - healthBarWidthOffset,
				boxCords[1].y + healthBarHeightOffset,
				boxCords[0].x - healthBarWidthOffset + healthBarWidth,
				boxCords[1].y + healthBarHeightOffset + healthBarHeight,
				Color32(0, 0, 0, 255)
			);
			pDebugRenderer->drawRect2d(
				boxCords[0].x - healthBarWidthOffset,
				boxCords[1].y + healthBarHeightOffset,
				boxCords[0].x - healthBarWidthOffset + healthBarPercWidth,
				boxCords[1].y + healthBarHeightOffset + healthBarHeight,
				healthBarColor
			);
		}
	}

	free(localPlayersBuffer);
}

HRESULT hkPresent(IDXGISwapChain* pThis, UINT SyncInterval, UINT Flags)
{
	const auto pDxRenderer = DxRenderer::GetInstance();
	const auto pGameRenderer = GameRenderer::GetInstance();
	if (pGameRenderer && pDxRenderer) {
		dxHeight = DxRenderer::GetInstance()->m_pScreen->m_Height;
		dxWidth = DxRenderer::GetInstance()->m_pScreen->m_Width;
		m_ViewProj = GameRenderer::GetInstance()->m_pRenderView->m_ViewProjection;

		UpdateCleanFrame(pThis, pDxRenderer->m_pDevice, pDxRenderer->m_pContext);

		ssMutex.lock();
		if (!pendingPbSs && !pendingFfSs) {
			cleanFrames = 0;
			DrawESP();
		}
		else
			cleanFrames++;
		ssMutex.unlock();
	}
	return oPresent(pThis, SyncInterval, Flags);
}

void hkCopySubresourceRegion(ID3D11DeviceContext* pContext,
	ID3D11Resource* pDstResource,
	UINT            DstSubresource,
	UINT            DstX,
	UINT            DstY,
	UINT            DstZ,
	ID3D11Resource* pSrcResource,
	UINT            SrcSubresource,
	const D3D11_BOX* pSrcBox) {

	void* RetAddress = _ReturnAddress();

	if (reinterpret_cast<DWORD_PTR>(RetAddress) == OFFSET_PBSSRETURN) {
		printf(xorstr_("Punkbuster Screenshot initiated!\n"));
		ssMutex.lock();
		if (!pCleanScreenShot) {
			ssMutex.unlock();
			return;
		}
		oCopySubresourceRegion(pContext, pDstResource, DstSubresource, DstX, DstY, DstZ, pCleanScreenShot, SrcSubresource, pSrcBox);
		ssMutex.unlock();
	} 
	else
		oCopySubresourceRegion(pContext, pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox);
}

void __fastcall hkDispatchMessage(MessageManager* pMessageManagerImpl, Message* pMessage) {
	auto type = pMessage->m_Type;

	if (pMessage->m_Type == VeniceNetworkRequestScreenshotMessage::Type() ||
		pMessage->m_Type == VeniceNetworkRequestFrontBufferScreenshotMessage::Type() ||
		pMessage->m_Type == VeniceNetworkRequestFrontBufferScreenshot2Message::Type()) {

		auto t = (VeniceNetworkRequestScreenshotMessage*)pMessage;
		printf(xorstr_("FairFight Screenshot initiated: %s ( %dx%d )\n"), pMessage->getType()->m_InfoData->m_Name, t->width, t->height);

		ssMutex.lock();
		pendingFfSs = true;
		ffSsMsg = t->clone(0x1425FDE60);
		ssMutex.unlock();

		return;
	}

	return oDispatchMessage(pMessageManagerImpl, pMessage);
}

void hkPreFrame(uintptr_t pthis, uint64_t a2)
{
	oPreFrameUpdate(pthis, a2);
	UpdatePreFrameData();
}

DWORD WINAPI InitThread(LPVOID reserved) noexcept
{
	AllocConsole();
	AttachConsole(GetCurrentProcessId());
	FILE* out{};
	freopen_s(&out, "CONOUT$", "w", stdout);

	DxRenderer* pDxRenderer;
	BorderInputNode* pBorderInputNode;
	Screen* pScreen;

	pDxRenderer = DxRenderer::GetInstance();
	printf("pRenderer = %p\n", pDxRenderer);
	if (!pDxRenderer)
		goto clean;

	pBorderInputNode = BorderInputNode::GetInstance();
	printf("pBorderInputNode = %p\n", pBorderInputNode);
	if (!pBorderInputNode)
		goto clean;

	playersBuffer = (PlayersLogicData*)malloc(sizeof(PlayersLogicData) * 70);
	playersCount = 0;

	MH_Initialize();

	MH_CreateHook((*reinterpret_cast<void***>(pDxRenderer->m_pScreen->m_pSwapChain))[8], hkPresent, reinterpret_cast<PVOID*>(&oPresent));
	MH_EnableHook((*reinterpret_cast<void***>(pDxRenderer->m_pScreen->m_pSwapChain))[8]);

	MH_CreateHook((*reinterpret_cast<void***>(pDxRenderer->m_pContext))[46], hkCopySubresourceRegion, reinterpret_cast<PVOID*>(&oCopySubresourceRegion));
	MH_EnableHook((*reinterpret_cast<void***>(pDxRenderer->m_pContext))[46]);

	MH_CreateHook((void*)OFFSET_DISPATCHMESSAGE, hkDispatchMessage, reinterpret_cast<PVOID*>(&oDispatchMessage));
	MH_EnableHook((void*)OFFSET_DISPATCHMESSAGE);

	oPreFrameUpdate = reinterpret_cast<PreFrameUpdate_t>(HookVTableFunction(reinterpret_cast<PDWORD64*>(pBorderInputNode->m_Vtable), reinterpret_cast<PBYTE>(&hkPreFrame), 3));

	return TRUE;

clean:
	FreeConsole();
	FreeLibraryAndExitThread(reinterpret_cast<HMODULE>(reserved), 0);
	return NULL;
}

void Deattach() {
	FreeConsole();

	MH_DisableHook(MH_ALL_HOOKS);
	HookVTableFunction(reinterpret_cast<PDWORD64*>(BorderInputNode::GetInstance()->m_Vtable), reinterpret_cast<PBYTE>(oPreFrameUpdate), 3);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, [[maybe_unused]] LPVOID lpvReserved)
{
	if (fdwReason == DLL_PROCESS_ATTACH) {
		CloseHandle(CreateThread(nullptr, 0, InitThread, hinstDLL, 0, nullptr));
	}
	if (fdwReason == DLL_PROCESS_DETACH) {
		Deattach();
	}
	return TRUE;
}