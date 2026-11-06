#pragma once

#include <d3d11_4.h> // ID3D11Multithread

namespace dx
{
	extern ID3D11Device* device;
	extern ID3D11DeviceContext* deviceContext;
	extern ID3D11Multithread* multithread;

	extern ID3D12Device* d3d12Device;
	extern ID3D12CommandQueue* d3d12CommandQueue;
}
