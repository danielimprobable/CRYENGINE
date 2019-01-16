// Copyright 2001-2018 Crytek GmbH / Crytek Group. All rights reserved.

#include "StdAfx.h"
#include <CryMath/Range.h>
#include "ShadowMap.h"
#include "HeightMapAO.h"
#include "D3DPostProcess.h"

#include "Common/Include_HLSL_CPP_Shared.h"
#include "Common/GraphicsPipelineStateSet.h"
#include "Common/TypedConstantBuffer.h"
#include "Common/Textures/TextureHelpers.h"
#include "Common/RenderView.h"
#include "Common/ReverseDepth.h"
#include "CompiledRenderObject.h"

#if defined(FEATURE_SVO_GI)
	#include "D3D_SVO.h"
#endif

// *INDENT-OFF*
ETEX_Format CShadowMapStage::GetShadowTexFormat(const SShadowConfig& shadowConfig, EPass passID) const
{
	switch(passID)
	{
	case ePass_DirectionalLight:
	case ePass_DirectionalLightRSM:
		return CRendererResources::s_hwTexFormatSupport.GetClosestFormatSupported(
		     shadowConfig.nTexFormat == 0 ? eTF_D32F  :
		    (shadowConfig.nTexFormat == 1 ? eTF_D16  : eTF_D24S8));

	case ePass_DirectionalLightCached:
		return CRendererResources::s_hwTexFormatSupport.GetClosestFormatSupported(
			shadowConfig.nCacheFormat == 0 ? eTF_D32F : eTF_D16);

	case ePass_LocalLightRSM:
	case ePass_LocalLight:
		return CRendererResources::s_hwTexFormatSupport.GetClosestFormatSupported(
			shadowConfig.nTexFormat == 0 ? eTF_D32F : eTF_D16);
	}

	return eTF_Unknown;
}
// *INDENT-ON*


CShadowMapStage::CShadowMapStage() 
	: m_perPassResources()
	, m_shadowsLocalLightsLinearizeDepth(1)
{}

void CShadowMapStage::Init()
{
	// init per pass resource set template
	{
		const EShaderStage shaderStages = EShaderStage_Vertex | EShaderStage_Hull | EShaderStage_Domain | EShaderStage_Pixel;

		m_perPassResources.SetTexture(EPerPassTexture_PerlinNoiseMap, CRendererResources::s_pTexNULL, EDefaultResourceViews::Default, EShaderStage_Vertex);
		m_perPassResources.SetTexture(EPerPassTexture_WindGrid      , CRendererResources::s_pTexNULL, EDefaultResourceViews::Default, EShaderStage_Vertex);
		m_perPassResources.SetTexture(EPerPassTexture_TerrainElevMap, CRendererResources::s_pTexNULL, EDefaultResourceViews::Default, EShaderStage_Vertex);
		m_perPassResources.SetTexture(EPerPassTexture_TerrainBaseMap, CRendererResources::s_pTexNULL, EDefaultResourceViews::sRGB   , EShaderStage_Pixel);

		m_perPassResources.SetConstantBuffer(eConstantBufferShaderSlot_PerPass, CDeviceBufferManager::GetNullConstantBuffer(), shaderStages);
		m_perPassResources.SetConstantBuffer(eConstantBufferShaderSlot_PerView, CDeviceBufferManager::GetNullConstantBuffer(), shaderStages);

		auto materialSamplers = gcpRendD3D->GetGraphicsPipeline().GetDefaultMaterialSamplers();
		for (size_t i = 0; i < materialSamplers.size(); ++i)
			m_perPassResources.SetSampler(EEfResSamplers(i), materialSamplers[i], shaderStages);

		// hardcoded point samplers
		m_perPassResources.SetSampler(8, EDefaultSamplerStates::PointWrap, shaderStages);
		m_perPassResources.SetSampler(9, EDefaultSamplerStates::PointClamp, shaderStages);
	}

	// Create resource layout
	m_pResourceLayout = gcpRendD3D->GetGraphicsPipeline().CreateScenePassLayout(m_perPassResources);

	// Freeze resource-set layout (assert will fire when violating the constraint)
	m_perPassResources.AcceptChangedBindPoints();

	{
		int nShadowTexFormat = CRendererCVars::CV_r_shadowtexformat;

		int nShadowPoolSize = 2048;
		if (ICVar* pShadowsPoolSizeCVar = iConsole->GetCVar("e_ShadowsPoolSize"))
			nShadowPoolSize = pShadowsPoolSizeCVar->GetIVal();

		int nShadowCacheFormat = CRendererCVars::CV_r_ShadowsCacheFormat;
		int nShadowCacheLODs = CRendererCVars::CV_r_ShadowsCache;

		int nShadowCacheCascades = 0;
		if (ICVar* pGsmLodsVar = gEnv->pConsole->GetCVar("e_GsmLodsNum"))
			nShadowCacheCascades = pGsmLodsVar->GetIVal();
		else
			nShadowCacheCascades = gEnv->pSystem->GetConfigSpec() == CONFIG_LOW_SPEC ? 4 : 5;

		StaticArray<int, MAX_GSM_LODS_NUM> nResolutions = gRenDev->GetCachedShadowsResolution();

		ReAllocateResources({ nShadowTexFormat, nShadowPoolSize, nShadowCacheFormat, nShadowCacheLODs, nShadowCacheCascades, nResolutions });
	}

#if defined(FEATURE_SVO_GI)
	CSvoRenderer::GetRsmTextures(m_pRsmColorTex, m_pRsmNormalTex, m_pRsmPoolColorTex, m_pRsmPoolNormalTex);
	m_pRsmPoolDepth = CRendererResources::s_ptexRT_ShadowPool;

	if (!CTexture::IsTextureExist(m_pRsmColorTex))
	{
		m_pRsmPoolDepth = CTexture::GetOrCreateTextureObject("SVO_PRJ_DEPTH_DUMMY", 0, 0, 1,
			CRendererResources::s_ptexRT_ShadowPool->GetTextureType(),
			CRendererResources::s_ptexRT_ShadowPool->GetFlags(),
			CRendererResources::s_ptexRT_ShadowPool->GetDstFormat());
	}
#endif

	// preallocate typically used passes (NOTE: at least one pass is needed for PSO compilation)
	// *INDENT-OFF*
	m_ShadowMapPasses[ePass_DirectionalLight      ].Init(this, 8,  CRendererResources::s_ptexRT_ShadowPool, nullptr,            nullptr);
	m_ShadowMapPasses[ePass_DirectionalLightCached].Init(this, 8,  CRendererResources::s_ptexRT_ShadowPool, nullptr,            nullptr);
	m_ShadowMapPasses[ePass_LocalLight            ].Init(this, 16, CRendererResources::s_ptexRT_ShadowPool, nullptr,            nullptr);
	m_ShadowMapPasses[ePass_DirectionalLightRSM   ].Init(this, 1,  CRendererResources::s_ptexRT_ShadowPool, m_pRsmColorTex,     m_pRsmNormalTex);
	m_ShadowMapPasses[ePass_LocalLightRSM         ].Init(this, 1,  m_pRsmPoolDepth,                         m_pRsmPoolColorTex, m_pRsmPoolNormalTex);
	// *INDENT-ON*
}

void CShadowMapStage::ReAllocateResources(const SShadowConfig shadowConfig)
{
	ETEX_Format eShadowTexFormat = shadowConfig.nTexFormat == 1 ? eTF_D16 : eTF_D32F;

	// =======================================
	// resize shadow pool if required
	{
		const int shadowPoolSize = shadowConfig.nPoolSize;

		CRendererResources::s_ptexRT_ShadowPool->Invalidate(shadowPoolSize, shadowPoolSize, eShadowTexFormat);
		if (!CTexture::IsTextureExist(CRendererResources::s_ptexRT_ShadowPool))
		{
#if !defined(_RELEASE) && !CRY_PLATFORM_WINDOWS
			static int reallocationCount = 0;
			assert(reallocationCount == 0); // don't want any realloc on consoles
			++reallocationCount;
#endif

			CRendererResources::s_ptexRT_ShadowPool->CreateDepthStencil(eShadowTexFormat, ColorF(Clr_FarPlane.r, 5.f, 0.f, 0.f));
		}
	}

	// =======================================
	{
		const auto& nResolutions = shadowConfig.nCacheResolutions;
		const ETEX_Format texFormat = shadowConfig.nCacheFormat == 0 ? eTF_D32F : eTF_D16;
		const int cachedShadowsStart = clamp_tpl(shadowConfig.nCacheLODs, 0, MAX_GSM_LODS_NUM - 1);
		const int cachedCascadesCount = cachedShadowsStart > 0 ? clamp_tpl(shadowConfig.nCacheCascades - cachedShadowsStart + 1, 0, MAX_GSM_LODS_NUM) : 0;

		for (int i = 0; i < MAX_GSM_LODS_NUM; ++i)
		{
			_smart_ptr<CTexture>& pTx = m_ShadowMapCache[i];

			if (!pTx)
			{
				char szName[64];
				cry_sprintf(szName, "%CachedShadowMap_%d", i);

				pTx = CTexture::GetOrCreateDepthStencil(szName, nResolutions[i], nResolutions[i], Clr_FarPlane, eTT_2D, FT_DONT_STREAM, texFormat);
			}

			pTx->Invalidate(nResolutions[i], nResolutions[i], texFormat);

			// delete existing texture in case it's not needed anymore
			if (CTexture::IsTextureExist(pTx) && nResolutions[i] == 0)
				pTx->ReleaseDeviceTexture(false);

			// allocate texture directly for all cached cascades
			if (!CTexture::IsTextureExist(pTx) && nResolutions[i] > 0 && i < cachedCascadesCount)
			{
				CryLog("Allocating shadow map cache %d x %d: %.2f MB", nResolutions[i], nResolutions[i], sqr(nResolutions[i]) * CTexture::BitsPerPixel(texFormat) / (1024.f * 1024.f * 8.f));
				pTx->CreateDepthStencil(texFormat, Clr_FarPlane);
			}
		}
	}

	// =======================================
	// allocate shadow maps for dynamic frustums
	for (int lightType = ePass_DirectionalLight; lightType <= ePass_DirectionalLightCached; ++lightType)
	{
		for (auto& cachedPass : m_ShadowMapPasses[lightType])
		{
			if (SShadowFrustumToRender* pFrustumToRender = cachedPass.m_pFrustumToRender)
			{
				ShadowMapFrustum& frustum = *pFrustumToRender->pFrustum;

				if (frustum.m_eFrustumType == ShadowMapFrustum::e_GsmDynamic ||
					frustum.m_eFrustumType == ShadowMapFrustum::e_GsmDynamicDistance ||
					frustum.m_eFrustumType == ShadowMapFrustum::e_PerObject ||
					frustum.m_eFrustumType == ShadowMapFrustum::e_Nearest)
				{
					ETEX_Format texFormat = GetShadowTexFormat(shadowConfig, EPass(lightType));
					frustum.m_eReqTF = texFormat;

					if (cachedPass.m_pDepthTarget)
					{
						cachedPass.m_pDepthTarget->Invalidate(frustum.nTextureWidth, frustum.nTextureHeight, texFormat);
						if (!CTexture::IsTextureExist(cachedPass.m_pDepthTarget))
						{
							cachedPass.m_pDepthTarget->CreateDepthStencil(texFormat, Clr_FarPlane);
						}
					}
				}
			}
		}
	}

	// =======================================
	{
		CRendererResources::s_ptexFarPlane->Invalidate(8, 8, eShadowTexFormat); // 1x HTILE/DepthTile
		if (!CTexture::IsTextureExist(CRendererResources::s_ptexFarPlane))
		{
			CRendererResources::s_ptexFarPlane->CreateDepthStencil(eShadowTexFormat, Clr_FarPlane);
			CClearSurfacePass::Execute(CRendererResources::s_ptexFarPlane, CLEAR_ZBUFFER, Clr_FarPlane.r, Val_Unused);
		}
	}
}

void CShadowMapStage::OnEntityDeleted(IRenderNode* pRenderNode)
{
#if SHADOW_FRUSTUM_CLEANUP
	for (int lightType = ePass_DirectionalLight; lightType <= ePass_LocalLightRSM; ++lightType)
	{
		for (const auto& cachedPass : m_ShadowMapPasses[lightType])
		{
			if (SShadowFrustumToRender* pFrustumToRender = cachedPass.m_pFrustumToRender)
			{
				ShadowMapFrustum& frustum = *pFrustumToRender->pFrustum;

				if (frustum.pLightOwner == pRenderNode)
				{
					if (frustum.m_eFrustumType == ShadowMapFrustum::e_GsmDynamic ||
						frustum.m_eFrustumType == ShadowMapFrustum::e_GsmDynamicDistance ||
						frustum.m_eFrustumType == ShadowMapFrustum::e_PerObject ||
						frustum.m_eFrustumType == ShadowMapFrustum::e_Nearest)
					{
						cachedPass.m_pDepthTarget->ReleaseDeviceTexture(false);
					}
				}
			}
		}
	}
#endif
}

size_t CShadowMapStage::GetAllocatedMemory()
{
	std::map<const void*, size_t> textureSet;

	// Prevent re-use of textures to contribute more than once to allocated size
	for (int lightType = ePass_DirectionalLight; lightType <= ePass_LocalLightRSM; ++lightType)
	{
		for (const auto& cachedPass : m_ShadowMapPasses[lightType])
		{
			if (const auto* depthTarget = cachedPass.m_pDepthTarget.get())
				textureSet[depthTarget] = depthTarget->GetActualSize();
		}
	}

	size_t sizeSum = 0;
	for (auto stat : textureSet)
		sizeSum += stat.second;
	return sizeSum;
}

void CShadowMapStage::OnCVarsChanged(const CCVarUpdateRecorder& cvarUpdater)
{
	if (cvarUpdater.GetCVar("r_ShadowsLocalLightsLinearizeDepth"))
	{
		m_shadowsLocalLightsLinearizeDepth = cvarUpdater.GetCVar("r_ShadowsLocalLightsLinearizeDepth")->intValue;
	}

	if (cvarUpdater.GetCVar("r_ShadowTexFormat") ||
		cvarUpdater.GetCVar("e_ShadowsPoolSize") ||
		cvarUpdater.GetCVar("r_ShadowsCacheFormat") ||
		cvarUpdater.GetCVar("r_ShadowsCache") ||
		cvarUpdater.GetCVar("r_ShadowsCacheResolutions") ||
		cvarUpdater.GetCVar("e_GsmLodsNum"))
	{
		int nShadowTexFormat = CRendererCVars::CV_r_shadowtexformat;
		if (cvarUpdater.GetCVar("r_ShadowTexFormat"))
			nShadowTexFormat = cvarUpdater.GetCVar("r_ShadowTexFormat")->intValue;

		int nShadowPoolSize = 2048;
		if (cvarUpdater.GetCVar("e_ShadowsPoolSize"))
			nShadowPoolSize = cvarUpdater.GetCVar("e_ShadowsPoolSize")->intValue;
		else if (ICVar* pShadowsPoolSizeCVar = iConsole->GetCVar("e_ShadowsPoolSize"))
			nShadowPoolSize = pShadowsPoolSizeCVar->GetIVal();

		int nShadowCacheFormat = CRendererCVars::CV_r_ShadowsCacheFormat;
		if (cvarUpdater.GetCVar("r_ShadowsCacheFormat"))
			nShadowCacheFormat = cvarUpdater.GetCVar("r_ShadowsCacheFormat")->intValue;

		int nShadowCacheLODs = CRendererCVars::CV_r_ShadowsCache;
		if (cvarUpdater.GetCVar("r_ShadowsCache"))
			nShadowCacheLODs = cvarUpdater.GetCVar("r_ShadowsCache")->intValue;

		int nShadowCacheCascades = 0;
		if (cvarUpdater.GetCVar("e_GsmLodsNum"))
			nShadowCacheCascades = cvarUpdater.GetCVar("e_GsmLodsNum")->intValue;
		else if (ICVar* pGsmLodsVar = gEnv->pConsole->GetCVar("e_GsmLodsNum"))
			nShadowCacheCascades = pGsmLodsVar->GetIVal();
		else
			nShadowCacheCascades = gEnv->pSystem->GetConfigSpec() == CONFIG_LOW_SPEC ? 4 : 5;

		StaticArray<int, MAX_GSM_LODS_NUM> nResolutions = gRenDev->GetCachedShadowsResolution();

		ReAllocateResources({ nShadowTexFormat, nShadowPoolSize, nShadowCacheFormat, nShadowCacheLODs, nShadowCacheCascades, nResolutions });
	}
}

bool CShadowMapStage::CreatePipelineState(const SGraphicsPipelineStateDescription& description, EPass passID, CDeviceGraphicsPSOPtr& outPSO)
{
	outPSO = NULL;

	CShader* pShader = static_cast<CShader*>(description.shaderItem.m_pShader);
	SShaderTechnique* pTechnique = pShader->GetTechnique(description.shaderItem.m_nTechnique, description.technique, true);
	if (!pTechnique)
		return true;

	CShaderResources* pRes = static_cast<CShaderResources*>(description.shaderItem.m_pShaderResources);
	if (pRes->m_ResFlags & MTL_FLAG_NOSHADOW)
		return true;

	SShaderPass* pShaderPass = &pTechnique->m_Passes[0];
	uint64 objectFlags = description.objectFlags;

	CDeviceGraphicsPSODesc psoDesc(m_pResourceLayout, description);
	psoDesc.m_bDynamicDepthBias = true;

	// Handle quality flags
	CStandardGraphicsPipeline::ApplyShaderQuality(psoDesc, gcpRendD3D->GetShaderProfile(pShader->m_eShaderType));

	///////////////////////////////////
	//SStateRaster curRS = rd->m_StatesRS[rd->m_nCurStateRS];
	//if (CV_r_ShadowGenDepthClip==0)
	//{
	//	if( rTI.m_vFrustumInfo.x > 100.0f )
	//	{
	//		SStateRaster customRS = curRS;
	//		customRS.Desc.DepthClipEnable = false;
	//		rd->SetRasterState(&customRS);
	//	}
	//}

	// Set resource states
	bool bTwoSided = false;
	{
		if (pRes->m_ResFlags & MTL_FLAG_2SIDED)
			bTwoSided = true;

		if (pRes->IsAlphaTested())
			psoDesc.m_ShaderFlags_RT |= g_HWSR_MaskBit[HWSR_ALPHATEST];

		if (passID == ePass_DirectionalLightRSM || passID == ePass_LocalLightRSM)
		{
			if (pRes->m_Textures[EFTT_DIFFUSE] && pRes->m_Textures[EFTT_DIFFUSE]->m_Ext.m_pTexModifier)
				psoDesc.m_ShaderFlags_MD |= pRes->m_Textures[EFTT_DIFFUSE]->m_Ext.m_nUpdateFlags;
		}

		// Merge EDeformType into EVertexModifier to save space/parameters
		if (pRes->m_pDeformInfo)
			psoDesc.m_ShaderFlags_MDV |= EVertexModifier(pRes->m_pDeformInfo->m_eType);
	}

	if (m_shadowsLocalLightsLinearizeDepth == 1)
	{
		psoDesc.m_ShaderFlags_RT |= g_HWSR_MaskBit[HWSR_SHADOW_DEPTH_OUTPUT_LINEAR];
	}

	//tessellation
	psoDesc.m_bAllowTesselation = false;
	psoDesc.m_ShaderFlags_RT |= g_HWSR_MaskBit[HWSR_NO_TESSELLATION];

#ifdef TESSELLATION_RENDERER
	const bool bHasTesselationShaders = pShaderPass && pShaderPass->m_HShader && pShaderPass->m_DShader;
	if (bHasTesselationShaders && (!(objectFlags & FOB_NEAREST) && (objectFlags & FOB_ALLOW_TESSELLATION)))
	{
		psoDesc.m_ShaderFlags_RT &= ~g_HWSR_MaskBit[HWSR_NO_TESSELLATION];
		psoDesc.m_bAllowTesselation = true;
	}
#endif

	psoDesc.m_CullMode = bTwoSided ? eCULL_None : ((pShaderPass && pShaderPass->m_eCull != -1) ? (ECull)pShaderPass->m_eCull : eCULL_Back);
	if (pShader->m_eSHDType == eSHDT_Terrain)
	{
		//Flipped matrix for point light sources
		if (passID == ePass_DirectionalLight || passID == ePass_DirectionalLightCached)
			psoDesc.m_CullMode = eCULL_None;
		else
			psoDesc.m_CullMode = eCULL_Front; //front faces culling by default for terrain
	}

	if (passID == ePass_DirectionalLight || passID == ePass_DirectionalLightCached || passID == ePass_DirectionalLightRSM)
	{
		psoDesc.m_ShaderFlags_RT |= g_HWSR_MaskBit[HWSR_HW_PCF_COMPARE];
	}
	else if (passID == ePass_LocalLight || passID == ePass_LocalLightRSM)
	{
		psoDesc.m_ShaderFlags_RT |= g_HWSR_MaskBit[HWSR_HW_PCF_COMPARE];
		psoDesc.m_ShaderFlags_RT |= g_HWSR_MaskBit[HWSR_CUBEMAP0];

		// RBPF_MIRRORCULL
		if (psoDesc.m_CullMode != eCULL_None)
		{
			psoDesc.m_CullMode = (psoDesc.m_CullMode == eCULL_Front) ? eCULL_Back : eCULL_Front;
		}
	}

	if (passID == ePass_DirectionalLightRSM || passID == ePass_LocalLightRSM)
	{
		psoDesc.m_ShaderFlags_RT |= g_HWSR_MaskBit[HWSR_SAMPLE4];

		if (!bTwoSided && psoDesc.m_CullMode == eCULL_Front)
			psoDesc.m_CullMode = eCULL_Back;

		if (objectFlags & FOB_DECAL_TEXGEN_2D)
			psoDesc.m_ShaderFlags_RT |= g_HWSR_MaskBit[HWSR_DECAL_TEXGEN_2D];

		if ((objectFlags & FOB_BLEND_WITH_TERRAIN_COLOR)) // && rRP.m_pCurObject->m_nTextureID > 0
			psoDesc.m_ShaderFlags_RT |= g_HWSR_MaskBit[HWSR_BLEND_WITH_TERRAIN_COLOR];
	}

	psoDesc.m_ShaderFlags_MDV |= pShader->m_nMDV;
	if (objectFlags & FOB_OWNER_GEOMETRY)
		psoDesc.m_ShaderFlags_MDV &= ~MDV_DEPTH_OFFSET;
	if (objectFlags & FOB_BENDED)
		psoDesc.m_ShaderFlags_MDV |= MDV_BENDING;

	if (!(objectFlags & FOB_TRANS_MASK))  //&& gRenDev->m_RP.m_RIs[0].Num() <= 1
		psoDesc.m_ShaderFlags_RT |= g_HWSR_MaskBit[HWSR_OBJ_IDENTITY];

	if (objectFlags & FOB_NEAREST)
		psoDesc.m_ShaderFlags_RT |= g_HWSR_MaskBit[HWSR_NEAREST];
	if (objectFlags & FOB_DISSOLVE)
		psoDesc.m_ShaderFlags_RT |= g_HWSR_MaskBit[HWSR_DISSOLVE];
	if (psoDesc.m_RenderState & GS_ALPHATEST)
		psoDesc.m_ShaderFlags_RT |= g_HWSR_MaskBit[HWSR_ALPHATEST];

	if (psoDesc.m_bAllowTesselation)
	{
		psoDesc.m_PrimitiveType = ept3ControlPointPatchList;
		psoDesc.m_ObjectStreamMask |= VSM_NORMALS;
	}

	// rendertarget and depth stencil format
	psoDesc.m_pRenderPass = m_ShadowMapPasses[passID][0].GetRenderPass();
	
#if (CRY_RENDERER_DIRECT3D >= 120)
	// emulate slope scaled bias in shader
	if (passID == ePass_DirectionalLight || passID == ePass_DirectionalLightCached || passID == ePass_DirectionalLightRSM)
	{
		psoDesc.m_ShaderFlags_RT |= g_HWSR_MaskBit[HWSR_SAMPLE0];
	}
#endif

	// Create PSO
	outPSO = GetDeviceObjectFactory().CreateGraphicsPSO(psoDesc);
	return outPSO != nullptr;
}

bool CShadowMapStage::CreatePipelineStates(DevicePipelineStatesArray* pStateArray, const SGraphicsPipelineStateDescription& stateDesc, CGraphicsPipelineStateLocalCache* pStateCache)
{
	DevicePipelineStatesArray& stageStates = pStateArray[m_stageID];

	if (pStateCache->Find(stateDesc, stageStates))
		return true;

	bool bFullyCompiled = true;

	for (EPass passID = ePass_First; passID < ePass_Count; passID = EPass(passID + 1))
	{
		assert(passID < stageStates.size());
		bFullyCompiled &= CreatePipelineState(stateDesc, passID, stageStates[passID]);
	}

	if (bFullyCompiled)
	{
		pStateCache->Put(stateDesc, stageStates);
	}

	return bFullyCompiled;
}

bool CShadowMapStage::CanRenderCachedShadows(const CCompiledRenderObject *obj) const
{
	return obj && obj->m_pso[m_stageID][ePass_DirectionalLightCached] && obj->m_pso[m_stageID][ePass_DirectionalLightCached]->IsValid();
}

void CShadowMapStage::Update()
{
	CRenderView* pRenderView = RenderView();

	if (pRenderView->IsRecursive() || pRenderView->GetCurrentEye() != CCamera::eEye_Left)
		return; // TODO: how will we handle recursion?

	{
		PROFILE_LABEL_SCOPE("SHADOWMAP_PREPARE");

		// prepare the shadow pool
		CDeferredShading::Instance().SetupPasses(pRenderView);

		// now prepare passes for each frustum
		for (auto& passGroup : m_ShadowMapPasses)
			passGroup.Reset();

		for (auto frustumType  = CRenderView::eShadowFrustumRenderType_First;
			      frustumType != CRenderView::eShadowFrustumRenderType_Count;
			      frustumType  = CRenderView::eShadowFrustumRenderType(frustumType + 1))
		{
			for (auto& pFrustumToRender : pRenderView->GetShadowFrustumsByType(frustumType))
			{
				CRY_ASSERT(pRenderView->GetFrameId() == pFrustumToRender->pShadowsView->GetFrameId());
				PrepareShadowPasses(*pFrustumToRender, frustumType);
			}
		}

		// clear the shadow maps we will use
		if (CRendererCVars::CV_r_ShadowMapsUpdate)
		{
			ClearShadowMaps(m_ShadowMapPasses);
		}
	}
}

void CShadowMapStage::PrepareShadowPasses(SShadowFrustumToRender& frustumToRender, CRenderView::eShadowFrustumRenderType frustumRenderType)
{
	const auto* pMainView = RenderView();
	auto* pShadowView = reinterpret_cast<CRenderView*>(frustumToRender.pShadowsView.get());
	auto* pFrustum = frustumToRender.pFrustum.get();

	ProfileLabel profileLabel;
	EPass passID;
	PreparePassIDForFrustum(frustumToRender, frustumRenderType, passID, profileLabel);

	const auto nSides = frustumToRender.pFrustum->GetNumSides();
	for (int side = 0; side < nSides; side++)
	{
		// assign empty shadow map
		frustumToRender.pFrustum->pDepthTex = CRendererResources::s_ptexFarPlane;

		if (pFrustum->ShouldSampleSide(side))
		{
			CShadowMapPass& curPass = m_ShadowMapPasses[passID].AddPass();
			cry_strcpy(curPass.m_ProfileLabel, profileLabel);

			if (PrepareOutputsForPass(frustumToRender, side, curPass))
			{
				curPass.m_pFrustumToRender = &frustumToRender;
				curPass.m_nShadowFrustumSide = side;
				curPass.m_eShadowPassID = passID;

				PrepareShadowPassForFrustum(frustumToRender, side, curPass);
				UpdateShadowFrustumFromPass(curPass, *pFrustum);

				curPass.m_bRequiresRender =
				  (CRendererCVars::CV_r_ShadowMapsUpdate && !pShadowView->GetRenderItems(ERenderListID(side)).empty()) ||
				   pFrustum->m_eFrustumType == ShadowMapFrustum::e_GsmDynamicDistance ||
				  (pFrustum->m_eFrustumType == ShadowMapFrustum::e_GsmCached && !pFrustum->bIncrementalUpdate);

				curPass.SetLabel(curPass.m_ProfileLabel);
				curPass.SetPassResources(m_pResourceLayout, curPass.GetResources());

				curPass.PrepareResources(pMainView);
				curPass.PrepareRenderPassForUse(GetDeviceObjectFactory().GetCoreCommandList());
			}
			else
			{
				m_ShadowMapPasses[passID].UndoAddPass();
			}
		}
	}
}

void CShadowMapStage::PreparePassIDForFrustum(const SShadowFrustumToRender& frustumToRender, CRenderView::eShadowFrustumRenderType frustumRenderType, EPass& passID, ProfileLabel& profileLabel) const
{
	const ShadowMapFrustum& frustum = *frustumToRender.pFrustum;

#if defined(FEATURE_SVO_GI)
	if (CSvoRenderer::GetRsmColorMap(frustum, true) && CSvoRenderer::GetRsmNormlMap(frustum, true))
	{
		if (frustumRenderType == CRenderView::eShadowFrustumRenderType_SunDynamic)
		{
			passID = ePass_DirectionalLightRSM;
			cry_sprintf(profileLabel, "SUN FRUSTUM (RSM) %i", frustum.nShadowMapLod);
		}
		else
		{
			passID = ePass_LocalLightRSM;
			cry_sprintf(profileLabel, "LOCAL LIGHT (RSM) %s", frustumToRender.pLight->m_sName);
		}
	}
	else
#endif
	{
		switch (frustumRenderType)
		{
		case CRenderView::eShadowFrustumRenderType_SunDynamic:
			{
				passID = ePass_DirectionalLight;
				const char* szLabel = frustum.m_eFrustumType == ShadowMapFrustum::e_GsmDynamicDistance ? "SUN DISTANCE FRUSTUM %i" : "SUN FRUSTUM %i";
				cry_sprintf(profileLabel, szLabel, frustum.nShadowMapLod);
			}
			break;
		case CRenderView::eShadowFrustumRenderType_SunCached:
			{
				passID = ePass_DirectionalLightCached;
				cry_sprintf(profileLabel, "SUN CACHED %i", frustum.nShadowMapLod);
			}
			break;
		case CRenderView::eShadowFrustumRenderType_HeightmapAO:
			{
				passID = ePass_DirectionalLightCached;
				cry_sprintf(profileLabel, "HEIGHTMAP AO");
			}
			break;
		case CRenderView::eShadowFrustumRenderType_Custom:
			{
				passID = ePass_DirectionalLight;

				if (frustum.m_eFrustumType == ShadowMapFrustum::e_Nearest)
				{
					cry_sprintf(profileLabel, "SUN NEAREST");
				}
				else
				{
					cry_sprintf(profileLabel, "SUN PER OBJECT");
				}
			}
			break;
		case CRenderView::eShadowFrustumRenderType_LocalLight:
			{
				passID = ePass_LocalLight;
				cry_sprintf(profileLabel, "LOCAL LIGHT %s", frustumToRender.pLight->m_sName);
			}
			break;
		}
	}
}

bool CShadowMapStage::PrepareOutputsForPass(const SShadowFrustumToRender& frustumToRender, int nSide, CShadowMapPass& targetPass) const
{
	const ShadowMapFrustum& frustum = *frustumToRender.pFrustum;

	int arrViewport[4];
	_smart_ptr<CTexture> pDepthTarget;
	std::array<CTexture*, 2> colorTargets;
	colorTargets.fill(nullptr);
	const CShadowMapPass* pClearDepthMapProvider = nullptr;
	CShadowMapPass::eClearMode clearMode = CShadowMapPass::eClearMode_Fill;

	if (frustum.bUseShadowsPool)
	{
		CRY_ASSERT(!frustum.IsCached());

		pDepthTarget = CRendererResources::s_ptexRT_ShadowPool;
		clearMode = CShadowMapPass::eClearMode_FillRect;
		frustum.GetSideViewport(nSide, arrViewport);
	}
	else
	{
		char pName[256];
		cry_sprintf(pName, "$%s", targetPass.m_ProfileLabel);

		if (frustum.m_eFrustumType == ShadowMapFrustum::e_GsmDynamic ||
		    frustum.m_eFrustumType == ShadowMapFrustum::e_PerObject ||
		    frustum.m_eFrustumType == ShadowMapFrustum::e_Nearest)
		{
			pDepthTarget = CTexture::GetOrCreateDepthStencilPtr(pName, frustum.nTextureWidth, frustum.nTextureHeight, frustum.clearValue, frustum.m_eReqTT, FT_USAGE_TEMPORARY | FT_NOMIPS | FT_STATE_CLAMP, frustum.m_eReqTF);
		}
		else
		{
			pDepthTarget = PrepareOutputsForFrustumWithCaching(pName, frustum, pClearDepthMapProvider, clearMode);
		}

		arrViewport[0] = arrViewport[1] = 0;
		arrViewport[2] = pDepthTarget ? pDepthTarget->GetWidth() : 0;
		arrViewport[3] = pDepthTarget ? pDepthTarget->GetHeight() : 0;
	}

#if defined(FEATURE_SVO_GI)
	colorTargets[0] = CSvoRenderer::GetInstance()->GetRsmColorMap(frustum);
	colorTargets[1] = CSvoRenderer::GetInstance()->GetRsmNormlMap(frustum);
#endif

	targetPass.m_pDepthTarget = pDepthTarget;
	if (!pDepthTarget || !pDepthTarget->GetDevTexture())
		return false;

	// now apply to pass
	targetPass.m_pClearDepthMapProvider = pClearDepthMapProvider;
	targetPass.m_clearMode = clearMode;
	targetPass.SetRenderTargets(pDepthTarget, colorTargets[0], colorTargets[1]);

	D3DViewPort viewport = { float(arrViewport[0]), float(arrViewport[1]), float(arrViewport[2]), float(arrViewport[3]), 0, 1 };
	targetPass.SetViewport(viewport);

	return true;
}

void CShadowMapStage::UpdateShadowFrustumFromPass(const CShadowMapPass& sourcePass, ShadowMapFrustum& targetFrustum) const
{
	CTexture* pDepthTarget = sourcePass.GetPassDesc().GetDepthTarget().pTexture;

	if (targetFrustum.m_eFrustumType == ShadowMapFrustum::e_Nearest)
	{
		targetFrustum.fWidthS *= targetFrustum.nTexSize / float(pDepthTarget->GetWidth());
		targetFrustum.fWidthT *= targetFrustum.nTexSize / float(pDepthTarget->GetHeight());
	}

	targetFrustum.pDepthTex      = pDepthTarget;
	targetFrustum.nTextureWidth  = pDepthTarget->GetWidth();
	targetFrustum.nTextureHeight = pDepthTarget->GetHeight();
	targetFrustum.clearValue     = pDepthTarget->GetClearColor();

	targetFrustum.mLightViewMatrix = sourcePass.m_ViewProjMatrix;
	targetFrustum.mLightProjMatrix.SetIdentity();

	if (sourcePass.m_pClearDepthMapProvider)
	{
		const ShadowMapFrustum* pSrcFrustum = sourcePass.m_pClearDepthMapProvider->m_pFrustumToRender->pFrustum;

		targetFrustum.fNearDist = pSrcFrustum->fNearDist;
		targetFrustum.fFarDist = pSrcFrustum->fFarDist;
		targetFrustum.fRendNear = pSrcFrustum->fRendNear;
		targetFrustum.fDepthConstBias = pSrcFrustum->fDepthConstBias;
		targetFrustum.fDepthTestBias = pSrcFrustum->fDepthTestBias;
		targetFrustum.fDepthSlopeBias = pSrcFrustum->fDepthSlopeBias;
		targetFrustum.fDepthBiasClamp = pSrcFrustum->fDepthBiasClamp;
	}
}

_smart_ptr<CTexture> CShadowMapStage::PrepareOutputsForFrustumWithCaching(const char* pName, const ShadowMapFrustum& frustum, const CShadowMapPass*& pClearDepthMapProvider, CShadowMapPass::eClearMode& clearMode) const
{
	CRY_ASSERT(frustum.IsCached() || frustum.m_eFrustumType == ShadowMapFrustum::eFrustumType::e_GsmDynamicDistance);

	_smart_ptr<CTexture> pDepthTarget;
	pClearDepthMapProvider = nullptr;
	clearMode = CShadowMapPass::eClearMode_Fill;

	if (frustum.IsCached())
	{
		if (frustum.m_eFrustumType == ShadowMapFrustum::eFrustumType::e_GsmCached)
		{
			int nCachedMapIndex = frustum.nShadowCacheLod;
			CRY_ASSERT(nCachedMapIndex >= 0 && nCachedMapIndex < CRY_ARRAY_COUNT(m_ShadowMapCache));

			pDepthTarget = m_ShadowMapCache[clamp_tpl(nCachedMapIndex, 0, int(CRY_ARRAY_COUNT(m_ShadowMapCache) - 1))];
		}
		else if (frustum.m_eFrustumType == ShadowMapFrustum::eFrustumType::e_HeightMapAO)
		{
			auto* heightMapAO = GetStdGraphicsPipeline().GetHeightMapAOStage();
			CRY_ASSERT(heightMapAO->IsStageActive(EShaderRenderingFlags(0)));

			pDepthTarget = heightMapAO->GetHeightMapAOScreenDepthTex();
		}

		clearMode = frustum.bIncrementalUpdate ? CShadowMapPass::eClearMode_None : CShadowMapPass::eClearMode_Fill;
	}
	else if (frustum.m_eFrustumType == ShadowMapFrustum::eFrustumType::e_GsmDynamicDistance)
	{
		// find corresponding cached frustum
		for (const auto& cachedPass : m_ShadowMapPasses[ePass_DirectionalLightCached])
		{
			const ShadowMapFrustum* pCachedFrustum = cachedPass.m_pFrustumToRender->pFrustum;
			if (pCachedFrustum->nShadowMapLod == frustum.nShadowMapLod)
			{
				pClearDepthMapProvider = &cachedPass;
				clearMode = CShadowMapPass::eClearMode_CopyDepthMap;

				pDepthTarget = CTexture::GetOrCreateDepthStencilPtr(pName, frustum.nTextureWidth, frustum.nTextureHeight, frustum.clearValue, frustum.m_eReqTT, FT_USAGE_TEMPORARY | FT_NOMIPS | FT_STATE_CLAMP, frustum.m_eReqTF);
				break;
			}
		}
	}

	return pDepthTarget;
}

void CShadowMapStage::PrepareShadowPassForFrustum(const SShadowFrustumToRender& frustumToRender, int nSide, CShadowMapPass& targetPass) const
{
	const CRenderView* pMainView = RenderView();
	const ShadowMapFrustum& frustum = *frustumToRender.pFrustum;

	Vec4 frustumInfo = Vec4(
	  (CRendererCVars::CV_r_ShadowGenDepthClip == 0 && frustum.fRendNear > 0.0f) ? frustum.fRendNear : frustum.fNearDist,
	  (frustum.m_eFrustumType == ShadowMapFrustum::e_HeightMapAO) ? 1.0f : frustum.fFarDist,
	  frustum.fDepthSlopeBias,
	  frustum.fDepthTestBias
	  );

	// get view projection matrix
	if (!frustum.bOmniDirectionalShadow)
	{
		Matrix44A viewProj = frustum.mLightViewMatrix;

		if (frustum.m_eFrustumType == ShadowMapFrustum::e_Nearest)
		{
			const Vec3 camPos = pMainView->GetCamera(CCamera::eEye_Left).GetPosition();
			AABB aabb = frustum.aabbCasters;
			aabb.Move(camPos);

			Matrix44A view, proj;
			CShadowUtils::GetShadowMatrixForObject(proj, view, frustumInfo, frustum.vLightSrcRelPos, aabb);

			viewProj = view * proj;
		}

		Matrix44A viewProjOrig = viewProj;
		if (targetPass.m_pClearDepthMapProvider)
		{
			// If the sub-frustum falls out of the full frustum entirely, then no depth-copy is needed or possible
			if (!CShadowUtils::GetSubfrustumMatrix(viewProj, targetPass.m_pClearDepthMapProvider->m_pFrustumToRender->pFrustum, &frustum))
				targetPass.m_clearMode = CShadowMapPass::eClearMode_Fill;
		}

		targetPass.m_ViewProjMatrix = viewProj;
		targetPass.m_ViewProjMatrixOrig = viewProjOrig;
		targetPass.m_FrustumInfo = frustumInfo;
		targetPass.SetDepthBias(0.0f, frustumInfo.z, frustum.fDepthBiasClamp);
	}
	else
	{
		Matrix44 view, proj;
		CShadowUtils::GetCubemapFrustum(FTYP_SHADOWOMNIPROJECTION, &frustum, nSide, &proj, &view);

		targetPass.m_ViewProjMatrix = view * proj;
		targetPass.m_FrustumInfo = frustumInfo;
		targetPass.SetDepthBias(0.0f, 0.0f, 0.0f);
	}

	// Override clear mode for dynamic lights: Cached sides do not need a clear
	if (frustum.m_eFrustumType == ShadowMapFrustum::e_GsmDynamic && frustum.ShouldCacheSideHint(nSide))
		targetPass.m_clearMode = CShadowMapPass::eClearMode_None;
}

void CShadowMapStage::CShadowMapPassGroup::Init(CShadowMapStage* pStage, int nSize, CTexture* pDepthTarget, CTexture* pColorTarget0, CTexture* pColorTarget1)
{
	m_Passes.clear();
	m_Passes.reserve(nSize);
	m_PassCount = 0;
	m_pStage = pStage;

	for (int i = 0; i < nSize; ++i)
	{
		m_Passes.emplace_back(pStage);
		m_Passes.back().SetRenderTargets(pDepthTarget, pColorTarget0, pColorTarget1);
	}
}

CShadowMapStage::CShadowMapPass& CShadowMapStage::CShadowMapPassGroup::AddPass()
{ 
	if (m_PassCount >= GetCapacity())
		m_Passes.emplace_back(m_pStage);
	
	return m_Passes[m_PassCount++];
}

CShadowMapStage::CShadowMapPass::CShadowMapPass(CShadowMapStage* pStage)
	: m_perPassResources(pStage->m_perPassResources) // clone per pass resources from stage
	, m_ViewProjMatrix(IDENTITY)
	, m_ViewProjMatrixOrig(IDENTITY)
{
	m_pDepthTarget = nullptr;
	m_pFrustumToRender = nullptr;
	m_nShadowFrustumSide = 0;
	m_eShadowPassID = EPass(0);
	m_pShadowMapStage = pStage;

	m_pPerPassResourceSet = GetDeviceObjectFactory().CreateResourceSet(CDeviceResourceSet::EFlags_ForceSetAllState);
	m_pPerPassConstantBuffer = gcpRendD3D->m_DevBufMan.CreateConstantBuffer(sizeof(HLSL_PerPassConstantBuffer_ShadowGen));
	m_pPerViewConstantBuffer = gcpRendD3D->m_DevBufMan.CreateConstantBuffer(sizeof(HLSL_PerViewGlobalConstantBuffer));
}

CShadowMapStage::CShadowMapPass::CShadowMapPass(CShadowMapPass&& other)
	: CSceneRenderPass(std::move(other))
	, m_pFrustumToRender(std::move(other.m_pFrustumToRender))
	, m_nShadowFrustumSide(std::move(other.m_nShadowFrustumSide))
	, m_eShadowPassID(std::move(other.m_eShadowPassID))
	, m_bRequiresRender(std::move(other.m_bRequiresRender))
	, m_pPerPassConstantBuffer(std::move(other.m_pPerPassConstantBuffer))
	, m_pPerViewConstantBuffer(std::move(other.m_pPerViewConstantBuffer))
	, m_perPassResources(other.m_perPassResources)
	, m_ViewProjMatrix(std::move(other.m_ViewProjMatrix))
	, m_ViewProjMatrixOrig(std::move(other.m_ViewProjMatrixOrig))
	, m_FrustumInfo(std::move(other.m_FrustumInfo))
	, m_pShadowMapStage(std::move(other.m_pShadowMapStage ))
	, m_clearMode(std::move(other.m_clearMode))
	, m_pClearDepthMapProvider(std::move(other.m_pClearDepthMapProvider))
{
	strncpy(m_ProfileLabel, other.m_ProfileLabel, sizeof(m_ProfileLabel));
}

bool CShadowMapStage::CShadowMapPass::PrepareResources(const CRenderView* pMainView)
{
	CD3D9Renderer* pRenderer = gcpRendD3D;

	assert(m_pFrustumToRender);
	assert(m_pPerPassResourceSet);
	assert(m_pPerViewConstantBuffer);
	assert(m_pPerPassConstantBuffer);

	ShadowMapFrustum& frustum = *m_pFrustumToRender->pFrustum;

	// update per pass textures
	{
		int nTerrainTex0 = 0, nTerrainTex1 = 0, nTerrainTex2 = 0;
		ITerrain * pTerrain = gEnv->p3DEngine->GetITerrain();
		if (pTerrain)
			pTerrain->GetAtlasTexId(nTerrainTex0, nTerrainTex1, nTerrainTex2);

		m_perPassResources.SetTexture(EPerPassTexture_PerlinNoiseMap, CRendererResources::s_ptexPerlinNoiseMap, EDefaultResourceViews::Default, EShaderStage_Vertex);
		m_perPassResources.SetTexture(EPerPassTexture_WindGrid, CRendererResources::s_ptexWindGrid, EDefaultResourceViews::Default, EShaderStage_Vertex);
		m_perPassResources.SetTexture(EPerPassTexture_TerrainBaseMap, CTexture::GetByID(nTerrainTex0), EDefaultResourceViews::sRGB, EShaderStage_Pixel);
		m_perPassResources.SetTexture(EPerPassTexture_TerrainElevMap, CTexture::GetByID(nTerrainTex2), EDefaultResourceViews::Default, EShaderStage_Vertex);
	}

	// per pass CB
	{
		CTypedConstantBuffer<HLSL_PerPassConstantBuffer_ShadowGen, 256> cb(m_pPerPassConstantBuffer);

		cb->CP_ShadowGen_LightPos = Vec4(frustum.vLightSrcRelPos + frustum.vProjTranslation, 0);
		cb->CP_ShadowGen_ViewPos = Vec4(pMainView->GetCamera(CCamera::eEye_Left).GetPosition(), 0);
		cb->CP_ShadowGen_DepthTestBias = Vec4(ZERO); // TODO

		cb->CP_ShadowGen_FrustrumInfo = m_FrustumInfo;

		cb->CP_ShadowGen_VegetationAlphaClamp = Vec4(ZERO);
#if defined(FEATURE_SVO_GI)
		if (CSvoRenderer* pSvoRenderer = CSvoRenderer::GetInstance())
		{
			cb->CP_ShadowGen_VegetationAlphaClamp.x = pSvoRenderer->GetVegetationMaxOpacity();
		}
#endif

		cb.CopyToDevice();

		m_perPassResources.SetConstantBuffer(eConstantBufferShaderSlot_PerPass, m_pPerPassConstantBuffer.get(), EShaderStage_Vertex | EShaderStage_Hull | EShaderStage_Domain | EShaderStage_Pixel);
	}

	// per view CB
	{
		SRenderViewInfo viewInfo;
		viewInfo.pCamera = &pMainView->GetCamera(CCamera::eEye_Left);
		viewInfo.cameraProjZeroMatrix = m_ViewProjMatrix;
		viewInfo.cameraProjMatrix = m_ViewProjMatrix;
		viewInfo.cameraProjNearestMatrix = m_ViewProjMatrix;
		viewInfo.cameraOrigin = viewInfo.pCamera->GetPosition();
		viewInfo.projMatrix = m_ViewProjMatrix;
		viewInfo.prevCameraProjMatrix = m_ViewProjMatrix;
		viewInfo.prevCameraProjNearestMatrix = m_ViewProjMatrix;
		viewInfo.viewport.width  = m_renderPassDesc.GetDepthTarget().pTexture->GetWidth();
		viewInfo.viewport.height = m_renderPassDesc.GetDepthTarget().pTexture->GetHeight();
		viewInfo.downscaleFactor = Vec4(1);
		viewInfo.pFrustumPlanes = frustum.FrustumPlanes[0].GetFrustumPlane(0);

		gcpRendD3D->GetGraphicsPipeline().GeneratePerViewConstantBuffer(&viewInfo, 1, m_pPerViewConstantBuffer);

		m_perPassResources.SetConstantBuffer(eConstantBufferShaderSlot_PerView, m_pPerViewConstantBuffer.get(), EShaderStage_Vertex | EShaderStage_Hull | EShaderStage_Domain | EShaderStage_Pixel);
	}

	CRY_ASSERT(!m_perPassResources.HasChangedBindPoints()); // Cannot change resource layout after init. It is baked into the shaders
	m_pPerPassResourceSet->Update(m_perPassResources);
	CRY_ASSERT(m_pPerPassResourceSet->IsValid());
	return m_pPerPassResourceSet->IsValid();
}

void CShadowMapStage::CShadowMapPass::PreRender()
{
	if (m_clearMode == eClearMode_CopyDepthMap)
	{
		CRY_ASSERT(m_pClearDepthMapProvider);
		m_pShadowMapStage->CopyShadowMap(*m_pClearDepthMapProvider, *this);
	}
}

void CShadowMapStage::CopyShadowMap(const CShadowMapPass& sourcePass, CShadowMapPass& targetPass)
{
	ShadowMapFrustum* pDst = targetPass.m_pFrustumToRender->pFrustum;
	const ShadowMapFrustum* pSrc = sourcePass.m_pFrustumToRender->pFrustum;

	CRY_ASSERT(pSrc->m_eFrustumType == ShadowMapFrustum::e_GsmCached);
	CRY_ASSERT(pDst->m_eFrustumType == ShadowMapFrustum::e_GsmDynamicDistance);
	CRY_ASSERT(pSrc->nShadowMapLod == pDst->nShadowMapLod);

	const bool bEmptySrcFrustum = !pSrc->ShouldSample();
	const auto& renderItems = reinterpret_cast<CRenderView*>(targetPass.m_pFrustumToRender->pShadowsView.get())->GetRenderItems(ERenderListID(0));
	const auto& depthTarget = targetPass.GetPassDesc().GetDepthTarget();

	// do we need to merge static shadows into the dynamic shadow map?
	if (bEmptySrcFrustum || !renderItems.empty())
	{
		if (bEmptySrcFrustum)
		{
			CClearSurfacePass::Execute(depthTarget.pTexture, CLEAR_ZBUFFER | CLEAR_STENCIL, Clr_FarPlane.r, Val_Stencil);
		}
		else
		{
			static CCryNameTSCRC tech("ReprojectShadowMap");
			CShader* pShader = CShaderMan::s_ShaderShadowMaskGen;

			m_CopyShadowMapPass.SetDepthTarget(depthTarget.pTexture, depthTarget.view);
			m_CopyShadowMapPass.SetTechnique(pShader, tech, 0);
			m_CopyShadowMapPass.SetState(GS_DEPTHWRITE | GS_DEPTHFUNC_NOTEQUAL);
			m_CopyShadowMapPass.SetPrimitiveType(CRenderPrimitive::ePrim_ProceduralTriangle);
			m_CopyShadowMapPass.SetTextureSamplerPair(0, pSrc->pDepthTex, EDefaultSamplerStates::LinearClamp);
			m_CopyShadowMapPass.BeginConstantUpdate();

			Matrix44 mReprojDstToSrc = pDst->mLightViewMatrix.GetInverted() * pSrc->mLightViewMatrix;

			static CCryNameR paramReprojMatDstToSrc("g_mReprojDstToSrc");
			m_CopyShadowMapPass.SetConstantArray(paramReprojMatDstToSrc, (Vec4*) mReprojDstToSrc.GetData(), 4, eHWSC_Pixel);

			Matrix44 mReprojSrcToDst = pSrc->mLightViewMatrix.GetInverted() * pDst->mLightViewMatrix;
			static CCryNameR paramReprojMatSrcToDst("g_mReprojSrcToDst");
			m_CopyShadowMapPass.SetConstantArray(paramReprojMatSrcToDst, (Vec4*) mReprojSrcToDst.GetData(), 4, eHWSC_Pixel);

			m_CopyShadowMapPass.Execute();
		}

		pDst->shadowCascade = TRect_tpl<float>{ 0, 0, static_cast<float>(pDst->nTextureWidth), static_cast<float>(pDst->nTextureHeight) };
	}
	else
	{
		// get crop rectangle for projection
		Matrix44r mReproj = Matrix44r(targetPass.m_ViewProjMatrixOrig).GetInverted() * Matrix44r(pSrc->mLightViewMatrix);
		Vec4r srcClipPosTL = Vec4r(-1, -1, 0, 1) * mReproj;
		srcClipPosTL /= srcClipPosTL.w;

		const float fSnap = 2.0f / pSrc->pDepthTex->GetWidth();
		Vec4 crop = Vec4(
		  crop.x = fSnap * int(srcClipPosTL.x / fSnap),
		  crop.y = fSnap * int(srcClipPosTL.y / fSnap),
		  crop.z = 2.0f * pDst->nTextureWidth / float(pSrc->nTextureWidth),
		  crop.w = 2.0f * pDst->nTextureHeight / float(pSrc->nTextureHeight)
		  );

		pDst->shadowCascade.Min = {
			(crop.x * 0.5f + 0.5f) * pSrc->pDepthTex->GetWidth() + 0.5f,
			(-(crop.y + crop.w) * 0.5f + 0.5f) * pSrc->pDepthTex->GetHeight() + 0.5f
		};
		pDst->shadowCascade.Max = pDst->shadowCascade.Min + Vec2_tpl<float>{
			static_cast<float>(pDst->nTextureWidth),
			static_cast<float>(pDst->nTextureHeight)
		};

		pDst->pDepthTex = pSrc->pDepthTex;
		pDst->nTexSize = pSrc->nTexSize;
		pDst->nTextureWidth = pSrc->nTextureWidth;
		pDst->nTextureHeight = pSrc->nTextureHeight;
		pDst->clearValue = pSrc->clearValue;
	}

	pDst->bIncrementalUpdate = true;
	pDst->fNearDist = pSrc->fNearDist;
	pDst->fFarDist = pSrc->fFarDist;
	pDst->fRendNear = pSrc->fRendNear;
	pDst->fDepthConstBias = pSrc->fDepthConstBias;
	pDst->fDepthTestBias = pSrc->fDepthTestBias;
	pDst->fDepthSlopeBias = pSrc->fDepthSlopeBias;
	pDst->fDepthBiasClamp = pSrc->fDepthBiasClamp;
}

void CShadowMapStage::ClearShadowMaps(PassGroupList& shadowMapPasses)
{
	// clear shadow pool regions first
	if (shadowMapPasses[ePass_LocalLight].GetCount() > 0 || shadowMapPasses[ePass_LocalLightRSM].GetCount() > 0)
	{
		std::vector<D3DRectangle> clearDepthRects; clearDepthRects.reserve(64);
		std::vector<D3DRectangle> clearColorRects; clearColorRects.reserve(64);

		EPass passes[] = { ePass_LocalLight, ePass_LocalLightRSM };
		for (const auto& pass : passes)
		{
			for (const auto& localLightPass : shadowMapPasses[pass])
			{
				if (localLightPass.m_clearMode == CShadowMapPass::eClearMode_None)
					continue;

				CRY_ASSERT(localLightPass.GetPassDesc().GetDepthTarget().pTexture == CRendererResources::s_ptexRT_ShadowPool);
				CRY_ASSERT(localLightPass.m_clearMode == CShadowMapPass::eClearMode_FillRect);

				clearDepthRects.push_back(localLightPass.GetScissorRect());

				if (pass == ePass_LocalLightRSM)
				{
					clearColorRects.push_back(localLightPass.GetScissorRect());
				}
			}
		}

		if (!clearDepthRects.empty() || !clearColorRects.empty())
		{
			m_ClearShadowPoolDepthPass.Execute(CRendererResources::s_ptexRT_ShadowPool, CLEAR_ZBUFFER | CLEAR_STENCIL, 1.0f, 5, clearDepthRects.size(), clearDepthRects.data());

#if defined(FEATURE_SVO_GI)
			CTexture* pRsmColor = CSvoRenderer::GetInstance()->GetRsmPoolCol();
			CTexture* pRsmNormals = CSvoRenderer::GetInstance()->GetRsmPoolNor();

			if (CTexture::IsTextureExist(pRsmColor) && CTexture::IsTextureExist(pRsmNormals))
			{
				m_ClearShadowPoolColorPass.Execute(pRsmColor, Clr_Transparent, clearColorRects.size(), clearColorRects.data());
				m_ClearShadowPoolNormalsPass.Execute(pRsmNormals, Clr_Transparent, clearColorRects.size(), clearColorRects.data());
			}
#endif
		}

	}

	// clear remaining depth maps and prepare all passes for use
	CDeviceCommandListRef commandList = GetDeviceObjectFactory().GetCoreCommandList();

	for (auto& passGroup : shadowMapPasses)
	{
		for (auto& curPass : passGroup)
		{
			if (curPass.m_clearMode == CShadowMapPass::eClearMode_Fill)
			{
				const auto& depthTarget = curPass.GetPassDesc().GetDepthTarget();

				CClearSurfacePass::Execute(depthTarget.pTexture, CLEAR_ZBUFFER, Clr_FarPlane.r, Val_Unused);

				for (const auto& colorTarget : curPass.GetPassDesc().GetRenderTargets())
				{
					if (!colorTarget.pTexture)
						break;

					CClearSurfacePass::Execute(colorTarget.pTexture, Clr_Transparent);
				}
			}

			curPass.PrepareRenderPassForUse(commandList);
		}
	}
}

void CShadowMapStage::Execute()
{
	FUNCTION_PROFILER_RENDERER();
	PROFILE_LABEL_SCOPE("SHADOWMAPS");

	CD3D9Renderer* rd = gcpRendD3D;
	const int nThreadID = gRenDev->GetRenderThreadID();
	CRenderItemDrawer& rendItemDrawer = RenderView()->GetDrawer();

	// Cached shadow maps cannot run concurrent due to CopyShadowMap pass
	for (auto& curPass : m_ShadowMapPasses[ePass_DirectionalLightCached])
	{
		if (curPass.m_bRequiresRender)
		{
			rendItemDrawer.InitDrawSubmission();

			CRenderView* pShadowsView = reinterpret_cast<CRenderView*>(curPass.GetFrustum()->pShadowsView.get());

			curPass.PreRender();
			curPass.BeginExecution();
			curPass.SetupDrawContext(m_stageID, curPass.m_eShadowPassID, TTYPE_SHADOWGEN, 0);
			curPass.DrawRenderItems(pShadowsView, (ERenderListID)curPass.m_nShadowFrustumSide);
			curPass.EndExecution();

			rendItemDrawer.JobifyDrawSubmission();
			rendItemDrawer.WaitForDrawSubmission();
		}
	}

	rendItemDrawer.InitDrawSubmission();

	for (auto passGroup  = ePass_DirectionalLight; passGroup != ePass_Count; passGroup  = EPass(passGroup+1))
	{
		if (passGroup == ePass_DirectionalLightCached)
			continue;

		for (auto& curPass : m_ShadowMapPasses[passGroup])
		{
			if (curPass.m_bRequiresRender)
			{
				CRenderView* pShadowsView = reinterpret_cast<CRenderView*>(curPass.GetFrustum()->pShadowsView.get());

				curPass.PreRender();
				curPass.BeginExecution();
				curPass.SetupDrawContext(m_stageID, curPass.m_eShadowPassID, TTYPE_SHADOWGEN, 0);
				curPass.DrawRenderItems(pShadowsView, (ERenderListID)curPass.m_nShadowFrustumSide);
				curPass.EndExecution();
			}
		}
	}

	rendItemDrawer.JobifyDrawSubmission();
}
