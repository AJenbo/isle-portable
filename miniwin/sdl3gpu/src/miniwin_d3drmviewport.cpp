#include "SDL3/SDL_stdinc.h"
#include "miniwin_d3drm_sdl3gpu.h"
#include "miniwin_d3drmframe_sdl3gpu.h"
#include "miniwin_d3drmviewport_sdl3gpu.h"
#include "miniwin_p.h"

#include <SDL3/SDL.h>
#include <functional>

Direct3DRMViewport_SDL3GPUImpl::Direct3DRMViewport_SDL3GPUImpl(
	DWORD width,
	DWORD height,
	SDL_GPUDevice* device,
	SDL_GPUTexture* transferTexture,
	SDL_GPUTransferBuffer* downloadTransferBuffer,
	SDL_GPUGraphicsPipeline* pipeline
)
	: m_width(width), m_height(height), m_device(device), m_transferTexture(transferTexture),
	  m_downloadTransferBuffer(downloadTransferBuffer), m_pipeline(pipeline)
{
}

void Direct3DRMViewport_SDL3GPUImpl::FreeDeviceResources()
{
	SDL_ReleaseGPUBuffer(m_device, m_vertexBuffer);
	SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
}

Direct3DRMViewport_SDL3GPUImpl::~Direct3DRMViewport_SDL3GPUImpl()
{
	FreeDeviceResources();
}

static void D3DRMMatrixMultiply(D3DRMMATRIX4D out, const D3DRMMATRIX4D a, const D3DRMMATRIX4D b)
{
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			out[i][j] = 0.0f;
			for (int k = 0; k < 4; ++k) {
				out[i][j] += a[i][k] * b[k][j];
			}
		}
	}
}

static void D3DRMMatrixInvertOrthogonal(D3DRMMATRIX4D out, const D3DRMMATRIX4D m)
{
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			out[i][j] = m[j][i];
		}
	}

	out[0][3] = out[1][3] = out[2][3] = 0.f;
	out[3][3] = 1.f;

	D3DVECTOR t = {m[3][0], m[3][1], m[3][2]};

	out[3][0] = -(out[0][0] * t.x + out[1][0] * t.y + out[2][0] * t.z);
	out[3][1] = -(out[0][1] * t.x + out[1][1] * t.y + out[2][1] * t.z);
	out[3][2] = -(out[0][2] * t.x + out[1][2] * t.y + out[2][2] * t.z);
}

static void CalculateProjectionMatrix(D3DRMMATRIX4D Result, float field, float aspect, float near, float far)
{
	float f = near / field;
	float depth = far - near;

	D3DRMMATRIX4D perspective = {
		{f, 0, 0, 0},
		{0, f * aspect, 0, 0},
		{0, 0, far / depth, 1},
		{0, 0, (-near * far) / depth, 0},
	};

	memcpy(Result, &perspective, sizeof(D3DRMMATRIX4D));
}

static void ComputeFrameWorldMatrix(IDirect3DRMFrame* frame, D3DRMMATRIX4D out)
{
	D3DRMMATRIX4D acc = {{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}, {0.f, 0.f, 0.f, 1.f}};

	IDirect3DRMFrame* cur = frame;
	while (cur) {
		auto* impl = static_cast<Direct3DRMFrame_SDL3GPUImpl*>(cur);
		D3DRMMATRIX4D local;
		memcpy(local, impl->m_transform, sizeof(local));

		D3DRMMATRIX4D tmp;
		D3DRMMatrixMultiply(tmp, local, acc);
		memcpy(acc, tmp, sizeof(acc));

		if (cur == impl->m_parent) {
			break;
		}
		cur = impl->m_parent;
	}
	memcpy(out, acc, sizeof(acc));
}

HRESULT Direct3DRMViewport_SDL3GPUImpl::CollectSceneData()
{
	MINIWIN_NOT_IMPLEMENTED(); // Lights, camera, textures, materials

	std::vector<PositionColorVertex> verts;

	// Compute camera matrix
	D3DRMMATRIX4D cameraWorld, viewMatrix;
	ComputeFrameWorldMatrix(m_camera, cameraWorld);
	D3DRMMatrixInvertOrthogonal(viewMatrix, cameraWorld);

	std::function<void(IDirect3DRMFrame*, D3DRMMATRIX4D)> recurseFrame;

	recurseFrame = [&](IDirect3DRMFrame* frame, D3DRMMATRIX4D parentMatrix) {
		// Retrieve the current frame's transform
		Direct3DRMFrame_SDL3GPUImpl* frameImpl = static_cast<Direct3DRMFrame_SDL3GPUImpl*>(frame);
		D3DRMMATRIX4D localMatrix;
		memcpy(localMatrix, frameImpl->m_transform, sizeof(D3DRMMATRIX4D));

		// Compute combined world matrix: world = parent * local
		D3DRMMATRIX4D worldMatrix;
		D3DRMMatrixMultiply(worldMatrix, parentMatrix, localMatrix);

		IDirect3DRMVisualArray* va = nullptr;
		if (SUCCEEDED(frame->GetVisuals(&va)) && va) {
			DWORD n = va->GetSize();
			for (DWORD i = 0; i < n; ++i) {
				IDirect3DRMVisual* vis = nullptr;
				va->GetElement(i, &vis);
				if (!vis) {
					continue;
				}

				// Pull geometry from meshes
				IDirect3DRMMesh* mesh = nullptr;
				if (SUCCEEDED(vis->QueryInterface(IID_IDirect3DRMMesh, (void**) &mesh)) && mesh) {
					DWORD groupCount = mesh->GetGroupCount();
					for (DWORD gi = 0; gi < groupCount; ++gi) {
						DWORD vtxCount, faceCount, vpf, dataSize;
						mesh->GetGroup(gi, &vtxCount, &faceCount, &vpf, &dataSize, nullptr);

						std::vector<D3DRMVERTEX> d3dVerts(vtxCount);
						std::vector<DWORD> faces(faceCount * vpf);
						mesh->GetVertices(gi, 0, vtxCount, d3dVerts.data());
						mesh->GetGroup(gi, &vtxCount, &faceCount, &vpf, nullptr, faces.data());

						D3DCOLOR color = mesh->GetGroupColor(gi);

						for (DWORD fi = 0; fi < faceCount; ++fi) {
							for (int idx = 0; idx < vpf; ++idx) {
								auto& dv = d3dVerts[faces[fi * vpf + idx]];

								// Apply world transform to the vertex
								D3DVECTOR pos = dv.position;
								D3DVECTOR worldPos;
								worldPos.x = pos.x * worldMatrix[0][0] + pos.y * worldMatrix[1][0] +
											 pos.z * worldMatrix[2][0] + worldMatrix[3][0];
								worldPos.y = pos.x * worldMatrix[0][1] + pos.y * worldMatrix[1][1] +
											 pos.z * worldMatrix[2][1] + worldMatrix[3][1];
								worldPos.z = pos.x * worldMatrix[0][2] + pos.y * worldMatrix[1][2] +
											 pos.z * worldMatrix[2][2] + worldMatrix[3][2];

								// View transform
								D3DVECTOR viewPos;
								viewPos.x = worldPos.x * viewMatrix[0][0] + worldPos.y * viewMatrix[1][0] +
											worldPos.z * viewMatrix[2][0] + viewMatrix[3][0];
								viewPos.y = worldPos.x * viewMatrix[0][1] + worldPos.y * viewMatrix[1][1] +
											worldPos.z * viewMatrix[2][1] + viewMatrix[3][1];
								viewPos.z = worldPos.x * viewMatrix[0][2] + worldPos.y * viewMatrix[1][2] +
											worldPos.z * viewMatrix[2][2] + viewMatrix[3][2];

								PositionColorVertex vtx;
								vtx.x = viewPos.x;
								vtx.y = viewPos.y;
								vtx.z = viewPos.z;
								vtx.a = (color >> 24) & 0xFF;
								vtx.r = (color >> 16) & 0xFF;
								vtx.g = (color >> 8) & 0xFF;
								vtx.b = (color >> 0) & 0xFF;
								verts.push_back(vtx);
							}
						}
					}
					mesh->Release();
				}

				// Recurse into child frames
				IDirect3DRMFrame* childFrame = nullptr;
				if (SUCCEEDED(vis->QueryInterface(IID_IDirect3DRMFrame, (void**) &childFrame)) && childFrame) {
					recurseFrame(childFrame, worldMatrix);
					childFrame->Release();
				}

				vis->Release();
			}
			va->Release();
		}
	};

	D3DRMMATRIX4D identity = {{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}, {0.f, 0.f, 0.f, 1.f}};

	recurseFrame(m_rootFrame, identity);

	PushVertices(verts.data(), verts.size());

	CalculateProjectionMatrix(m_uniforms.perspective, m_field, (float) m_width / (float) m_height, m_front, m_back);

	return D3DRM_OK;
}

void Direct3DRMViewport_SDL3GPUImpl::PushVertices(const PositionColorVertex* vertices, size_t count)
{
	if (count > m_vertexBufferCount) {
		if (m_vertexBuffer) {
			SDL_ReleaseGPUBuffer(m_device, m_vertexBuffer);
		}
		SDL_GPUBufferCreateInfo bufferCreateInfo = {};
		bufferCreateInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
		bufferCreateInfo.size = static_cast<Uint32>(sizeof(PositionColorVertex) * count);
		m_vertexBuffer = SDL_CreateGPUBuffer(m_device, &bufferCreateInfo);
		m_vertexBufferCount = count;
	}

	m_vertexCount = count;
	if (!count) {
		return;
	}

	SDL_GPUTransferBufferCreateInfo transferCreateInfo = {};
	transferCreateInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transferCreateInfo.size = static_cast<Uint32>(sizeof(PositionColorVertex) * m_vertexCount);
	SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferCreateInfo);

	PositionColorVertex* transferData =
		(PositionColorVertex*) SDL_MapGPUTransferBuffer(m_device, transferBuffer, false);

	memcpy(transferData, vertices, m_vertexCount * sizeof(PositionColorVertex));

	SDL_UnmapGPUTransferBuffer(m_device, transferBuffer);

	// Upload the transfer data to the vertex buffer
	SDL_GPUCommandBuffer* uploadCmdBuf = SDL_AcquireGPUCommandBuffer(m_device);
	SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(uploadCmdBuf);
	SDL_GPUTransferBufferLocation transferLocation = {};
	transferLocation.transfer_buffer = transferBuffer;
	transferLocation.offset = 0;

	SDL_GPUBufferRegion bufferRegion = {};
	bufferRegion.buffer = m_vertexBuffer;
	bufferRegion.offset = 0;
	bufferRegion.size = static_cast<Uint32>(sizeof(PositionColorVertex) * m_vertexCount);

	SDL_UploadToGPUBuffer(copyPass, &transferLocation, &bufferRegion, false);

	SDL_EndGPUCopyPass(copyPass);
	SDL_SubmitGPUCommandBuffer(uploadCmdBuf);
	SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
}

HRESULT Direct3DRMViewport_SDL3GPUImpl::Render(IDirect3DRMFrame* rootFrame)
{
	if (!m_device) {
		return DDERR_GENERIC;
	}

	m_rootFrame = rootFrame;
	HRESULT success = CollectSceneData();
	if (success != DD_OK) {
		return success;
	}

	SDL_GPUCommandBuffer* cmdbuf = SDL_AcquireGPUCommandBuffer(m_device);
	if (cmdbuf == NULL) {
		return DDERR_GENERIC;
	}

	// Render the graphics
	SDL_GPUColorTargetInfo colorTargetInfo = {};
	colorTargetInfo.texture = m_transferTexture;
	// Make the render target transparent so we can combine it with the back buffer
	colorTargetInfo.clear_color = {0, 0, 0, 0};
	colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
	SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(cmdbuf, &colorTargetInfo, 1, NULL);
	SDL_BindGPUGraphicsPipeline(renderPass, m_pipeline);

	SDL_PushGPUVertexUniformData(cmdbuf, 0, &m_uniforms, sizeof(m_uniforms));

	if (m_vertexCount) {
		SDL_GPUBufferBinding vertexBufferBinding = {};
		vertexBufferBinding.buffer = m_vertexBuffer;
		vertexBufferBinding.offset = 0;
		SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBufferBinding, 1);
		SDL_DrawGPUPrimitives(renderPass, m_vertexCount, 1, 0, 0);
	}

	SDL_EndGPURenderPass(renderPass);

	// Download rendered image
	SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmdbuf);
	SDL_GPUTextureRegion region = {};
	region.texture = m_transferTexture;
	region.w = DDBackBuffer_SDL3GPU->w;
	region.h = DDBackBuffer_SDL3GPU->h;
	region.d = 1;
	SDL_GPUTextureTransferInfo transferInfo = {};
	transferInfo.transfer_buffer = m_downloadTransferBuffer;
	transferInfo.offset = 0;
	SDL_DownloadFromGPUTexture(copyPass, &region, &transferInfo);
	SDL_EndGPUCopyPass(copyPass);
	SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmdbuf);
	if (!SDL_WaitForGPUFences(m_device, true, &fence, 1)) {
		return DDERR_GENERIC;
	}
	SDL_ReleaseGPUFence(m_device, fence);
	void* downloadedData = SDL_MapGPUTransferBuffer(m_device, m_downloadTransferBuffer, false);
	if (!downloadedData) {
		return DDERR_GENERIC;
	}

	SDL_DestroySurface(m_renderedImage);
	m_renderedImage = SDL_CreateSurfaceFrom(
		DDBackBuffer_SDL3GPU->w,
		DDBackBuffer_SDL3GPU->h,
		SDL_PIXELFORMAT_ABGR8888,
		downloadedData,
		DDBackBuffer_SDL3GPU->w * 4
	);

	SDL_Surface* convertedRender = SDL_ConvertSurface(m_renderedImage, SDL_PIXELFORMAT_RGBA8888);
	SDL_DestroySurface(m_renderedImage);
	SDL_UnmapGPUTransferBuffer(m_device, m_downloadTransferBuffer);
	m_renderedImage = convertedRender;

	return ForceUpdate(0, 0, DDBackBuffer_SDL3GPU->w, DDBackBuffer_SDL3GPU->h);
}

HRESULT Direct3DRMViewport_SDL3GPUImpl::ForceUpdate(int x, int y, int w, int h)
{
	if (!m_renderedImage) {
		return DDERR_GENERIC;
	}
	// Blit the render back to our backbuffer
	SDL_Rect srcRect{0, 0, DDBackBuffer_SDL3GPU->w, DDBackBuffer_SDL3GPU->h};

	const SDL_PixelFormatDetails* details = SDL_GetPixelFormatDetails(DDBackBuffer_SDL3GPU->format);
	if (details->Amask != 0) {
		// Backbuffer supports transparnacy
		SDL_Surface* convertedRender = SDL_ConvertSurface(m_renderedImage, DDBackBuffer_SDL3GPU->format);
		SDL_DestroySurface(m_renderedImage);
		m_renderedImage = convertedRender;
		return DD_OK;
	}

	if (m_renderedImage->format == DDBackBuffer_SDL3GPU->format) {
		// No conversion needed
		SDL_BlitSurface(m_renderedImage, &srcRect, DDBackBuffer_SDL3GPU, &srcRect);
		return DD_OK;
	}

	// Convert backbuffer to a format that supports transparancy
	SDL_Surface* tempBackbuffer = SDL_ConvertSurface(DDBackBuffer_SDL3GPU, m_renderedImage->format);
	SDL_BlitSurface(m_renderedImage, &srcRect, tempBackbuffer, &srcRect);
	// Then convert the result back to the backbuffer format and write it back
	SDL_Surface* newBackBuffer = SDL_ConvertSurface(tempBackbuffer, DDBackBuffer_SDL3GPU->format);
	SDL_DestroySurface(tempBackbuffer);
	SDL_BlitSurface(newBackBuffer, &srcRect, DDBackBuffer_SDL3GPU, &srcRect);
	SDL_DestroySurface(newBackBuffer);
	return DD_OK;
}

HRESULT Direct3DRMViewport_SDL3GPUImpl::Clear()
{
	if (!DDBackBuffer_SDL3GPU) {
		return DDERR_GENERIC;
	}

	uint8_t r = (m_backgroundColor >> 16) & 0xFF;
	uint8_t g = (m_backgroundColor >> 8) & 0xFF;
	uint8_t b = m_backgroundColor & 0xFF;

	Uint32 color = SDL_MapRGB(SDL_GetPixelFormatDetails(DDBackBuffer_SDL3GPU->format), nullptr, r, g, b);
	SDL_FillSurfaceRect(DDBackBuffer_SDL3GPU, NULL, color);
	return DD_OK;
}

HRESULT Direct3DRMViewport_SDL3GPUImpl::SetCamera(IDirect3DRMFrame* camera)
{
	if (m_camera) {
		m_camera->Release();
	}
	if (camera) {
		camera->AddRef();
	}
	m_camera = camera;
	return DD_OK;
}

HRESULT Direct3DRMViewport_SDL3GPUImpl::GetCamera(IDirect3DRMFrame** camera)
{
	if (m_camera) {
		m_camera->AddRef();
	}
	*camera = m_camera;
	return DD_OK;
}

HRESULT Direct3DRMViewport_SDL3GPUImpl::SetProjection(D3DRMPROJECTIONTYPE type)
{
	return DD_OK;
}

D3DRMPROJECTIONTYPE Direct3DRMViewport_SDL3GPUImpl::GetProjection()
{
	return D3DRMPROJECTIONTYPE::PERSPECTIVE;
}

HRESULT Direct3DRMViewport_SDL3GPUImpl::SetFront(D3DVALUE z)
{
	m_front = z;
	return DD_OK;
}

D3DVALUE Direct3DRMViewport_SDL3GPUImpl::GetFront()
{
	return m_front;
}

HRESULT Direct3DRMViewport_SDL3GPUImpl::SetBack(D3DVALUE z)
{
	m_back = z;
	return DD_OK;
}

D3DVALUE Direct3DRMViewport_SDL3GPUImpl::GetBack()
{
	return m_back;
}

HRESULT Direct3DRMViewport_SDL3GPUImpl::SetField(D3DVALUE field)
{
	m_field = field;
	return DD_OK;
}

D3DVALUE Direct3DRMViewport_SDL3GPUImpl::GetField()
{
	return m_field;
}

DWORD Direct3DRMViewport_SDL3GPUImpl::GetWidth()
{
	return m_width;
}

DWORD Direct3DRMViewport_SDL3GPUImpl::GetHeight()
{
	return m_height;
}

HRESULT Direct3DRMViewport_SDL3GPUImpl::Transform(D3DRMVECTOR4D* screen, D3DVECTOR* world)
{
	MINIWIN_NOT_IMPLEMENTED();
	return DD_OK;
}

HRESULT Direct3DRMViewport_SDL3GPUImpl::InverseTransform(D3DVECTOR* world, D3DRMVECTOR4D* screen)
{
	MINIWIN_NOT_IMPLEMENTED();
	return DD_OK;
}

D3DVECTOR TransformPoint(const D3DRMMATRIX4D& mat, const D3DVECTOR& v)
{
	return {
		v.x * mat[0][0] + v.y * mat[1][0] + v.z * mat[2][0] + mat[3][0],
		v.x * mat[0][1] + v.y * mat[1][1] + v.z * mat[2][1] + mat[3][1],
		v.x * mat[0][2] + v.y * mat[1][2] + v.z * mat[2][2] + mat[3][2],
	};
}

bool RayIntersectsBox(
	const D3DVECTOR& origin,
	const D3DVECTOR& dir,
	const D3DRMBOX& box,
	const D3DRMMATRIX4D& matrix,
	float& outT
)
{
	// Transform box corners
	D3DVECTOR min = TransformPoint(matrix, box.min);
	D3DVECTOR max = TransformPoint(matrix, box.max);

	// AABB-ray intersection (simplified slab test)
	float tmin = (min.x - origin.x) / dir.x;
	float tmax = (max.x - origin.x) / dir.x;
	if (tmin > tmax) {
		std::swap(tmin, tmax);
	}

	float tymin = (min.y - origin.y) / dir.y;
	float tymax = (max.y - origin.y) / dir.y;
	if (tymin > tymax) {
		std::swap(tymin, tymax);
	}

	if ((tmin > tymax) || (tymin > tmax)) {
		return false;
	}
	if (tymin > tmin) {
		tmin = tymin;
	}
	if (tymax < tmax) {
		tmax = tymax;
	}

	float tzmin = (min.z - origin.z) / dir.z;
	float tzmax = (max.z - origin.z) / dir.z;
	if (tzmin > tzmax) {
		std::swap(tzmin, tzmax);
	}

	if ((tmin > tzmax) || (tzmin > tmax)) {
		return false;
	}
	if (tzmin > tmin) {
		tmin = tzmin;
	}
	if (tzmax < tmax) {
		tmax = tzmax;
	}

	outT = tmin;
	return true;
}

void Normalize(D3DVECTOR& v)
{
	float len = SDL_sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
	if (len > 0.0f) {
		v.x /= len;
		v.y /= len;
		v.z /= len;
	}
}

IDirect3DRMFrameArray* CreateFrameArrayFrom(IDirect3DRMFrame* frame)
{
	auto* arr = new Direct3DRMFrameArray_SDL3GPUImpl();
	arr->AddElement(frame);
	return arr;
}

bool D3DRMMatrixInvert(D3DRMMATRIX4D out, const D3DRMMATRIX4D in)
{
	float inv[16], det;
	float m[16];

	// Flatten matrix
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			m[i * 4 + j] = in[j][i]; // transpose for column-major to row-major
		}
	}

	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
			 m[13] * m[6] * m[11] - m[13] * m[7] * m[10];

	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
			 m[12] * m[6] * m[11] + m[12] * m[7] * m[10];

	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
			 m[12] * m[5] * m[11] - m[12] * m[7] * m[9];

	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
			  m[12] * m[5] * m[10] + m[12] * m[6] * m[9];

	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
			 m[13] * m[2] * m[11] + m[13] * m[3] * m[10];

	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
			 m[12] * m[2] * m[11] - m[12] * m[3] * m[10];

	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
			 m[12] * m[1] * m[11] + m[12] * m[3] * m[9];

	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
			  m[12] * m[1] * m[10] - m[12] * m[2] * m[9];

	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
			 m[13] * m[2] * m[7] - m[13] * m[3] * m[6];

	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
			 m[12] * m[2] * m[7] + m[12] * m[3] * m[6];

	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
			  m[12] * m[1] * m[7] - m[12] * m[3] * m[5];

	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
			  m[12] * m[1] * m[6] + m[12] * m[2] * m[5];

	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
			 m[9] * m[2] * m[7] + m[9] * m[3] * m[6];

	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
			 m[8] * m[2] * m[7] - m[8] * m[3] * m[6];

	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
			  m[8] * m[1] * m[7] + m[8] * m[3] * m[5];

	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] -
			  m[8] * m[2] * m[5];

	det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

	if (det == 0.0f) {
		return false;
	}

	det = 1.0f / det;

	for (int i = 0; i < 16; i++) {
		inv[i] *= det;
	}

	// Store back to D3DRMMATRIX4D (column-major)
	for (int i = 0; i < 4; ++i) {
		for (int j = 0; j < 4; ++j) {
			out[j][i] = inv[i * 4 + j];
		}
	}

	return true;
}

HRESULT Direct3DRMViewport_SDL3GPUImpl::Pick(float x, float y, LPDIRECT3DRMPICKEDARRAY* pickedArray)
{
	struct Hit {
		PickRecord record;
		float distance;
	};

	std::vector<Hit> hits;

	// Step 1: Generate the ray in world space
	D3DRMMATRIX4D cameraWorld;
	ComputeFrameWorldMatrix(m_camera, cameraWorld);

	D3DVECTOR camPos = {cameraWorld[3][0], cameraWorld[3][1], cameraWorld[3][2]};

	// Convert screen (x,y) to normalized device coordinates
	float ndcX = (2.0f * x / m_width - 1.0f);
	float ndcY = (1.0f - 2.0f * y / m_height);

	// Get the inverse projection matrix
	D3DRMMATRIX4D projMatrix;
	CalculateProjectionMatrix(projMatrix, m_field, float(m_width) / float(m_height), m_front, m_back);

	D3DRMMATRIX4D invProj;
	D3DRMMatrixInvert(invProj, projMatrix);

	// Unproject the point (ndcX, ndcY, -1) to get the ray direction
	D3DVECTOR rayDir = {
		ndcX * invProj[0][0] + ndcY * invProj[1][0] + invProj[2][0],
		ndcX * invProj[0][1] + ndcY * invProj[1][1] + invProj[2][1],
		ndcX * invProj[0][2] + ndcY * invProj[1][2] + invProj[2][2],
	};

	Normalize(rayDir);

	std::function<void(IDirect3DRMFrame*, D3DRMMATRIX4D)> recurse;
	recurse = [&](IDirect3DRMFrame* frame, D3DRMMATRIX4D parentMatrix) {
		Direct3DRMFrame_SDL3GPUImpl* frameImpl = static_cast<Direct3DRMFrame_SDL3GPUImpl*>(frame);
		D3DRMMATRIX4D localMatrix;
		memcpy(localMatrix, frameImpl->m_transform, sizeof(D3DRMMATRIX4D));

		D3DRMMATRIX4D worldMatrix;
		D3DRMMatrixMultiply(worldMatrix, parentMatrix, localMatrix);

		IDirect3DRMVisualArray* visuals = nullptr;
		if (frame->GetVisuals(&visuals) != D3DRM_OK || !visuals) {
			return;
		}

		DWORD count = visuals->GetSize();
		for (DWORD i = 0; i < count; ++i) {
			IDirect3DRMVisual* vis = nullptr;
			visuals->GetElement(i, &vis);
			if (!vis) {
				continue;
			}

			IDirect3DRMMesh* mesh = nullptr;
			if (SUCCEEDED(vis->QueryInterface(IID_IDirect3DRMMesh, (void**) &mesh)) && mesh) {
				// Get bounding box and transform it
				D3DRMBOX box;
				if (SUCCEEDED(mesh->GetBox(&box))) {
					D3DVECTOR boxCenter = {
						(box.min.x + box.max.x) * 0.5f,
						(box.min.y + box.max.y) * 0.5f,
						(box.min.z + box.max.z) * 0.5f,
					};

					D3DVECTOR worldBoxCenter = TransformPoint(worldMatrix, boxCenter);

					float t = 0.0f;
					if (RayIntersectsBox(camPos, rayDir, box, worldMatrix, t)) {
						// Store hit
						PickRecord rec;
						rec.visual = vis;
						rec.frameArray = CreateFrameArrayFrom(frame);
						hits.push_back({rec, t});
					}
				}
				mesh->Release();
			}
			vis->Release();
		}

		visuals->Release();

		// Recurse into children
		IDirect3DRMFrameArray* children = nullptr;
		if (SUCCEEDED(frame->GetChildren(&children)) && children) {
			DWORD childCount = children->GetSize();
			for (DWORD i = 0; i < childCount; ++i) {
				IDirect3DRMFrame* child = nullptr;
				if (SUCCEEDED(children->GetElement(i, &child)) && child) {
					recurse(child, worldMatrix);
					child->Release();
				}
			}
			children->Release();
		}
	};

	D3DRMMATRIX4D identity = {{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}, {0.f, 0.f, 0.f, 1.f}};

	recurse(m_rootFrame, identity);

	// Sort by distance
	std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.distance < b.distance; });

	// Construct picked array
	std::vector<PickRecord> final;
	for (auto& h : hits) {
		final.push_back(h.record);
	}

	*pickedArray = new Direct3DRMPickedArray_SDL3GPUImpl(final.data(), final.size());

	return D3DRM_OK;
}

void Direct3DRMViewport_SDL3GPUImpl::CloseDevice()
{
	FreeDeviceResources();
	m_device = nullptr;
}
