// SPDX-License-Identifier: MIT
// The .asi half of FontEngine's texture upload.
//
// The ReShade build hands its atlases to ReShade's device API; here there is no ReShade, so the
// texture and its shader-resource view are created directly on the game's D3D11 device. The
// view pointer is what Dear ImGui wants as an ImTextureID, and imgui_impl_dx11 binds it as-is,
// so that is what the font engine gets back.
#ifndef TSRO_D3D11_FONT_SINK_HPP
#define TSRO_D3D11_FONT_SINK_HPP

#include <cstdint>
#include <map>

#include <d3d11.h>

#include "font_engine.hpp"

namespace tsro::asi {

class D3D11FontSink final : public overlay::FontTextureSink {
public:
    ~D3D11FontSink() override { release(); }

    /// Called whenever the swap chain's device is (re)discovered. A different device means
    /// every texture we hold belonged to something that no longer exists, so the map is dropped
    /// without releasing through the old device.
    void set_device(ID3D11Device* device) {
        if (device == device_) return;
        pairs_.clear();
        device_ = device;
    }

    /// Releases every live texture through the device that made them. Called before the device
    /// itself goes, never after.
    void release() {
        for (auto& [handle, texture] : pairs_) {
            reinterpret_cast<ID3D11ShaderResourceView*>(handle)->Release();
            texture->Release();
        }
        pairs_.clear();
    }

    std::uint64_t create(const unsigned char* rgba, int width, int height) override {
        if (device_ == nullptr || rgba == nullptr || width <= 0 || height <= 0) return 0;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;

        D3D11_SUBRESOURCE_DATA initial = {};
        initial.pSysMem = rgba;
        initial.SysMemPitch = static_cast<UINT>(width) * 4u;

        ID3D11Texture2D* texture = nullptr;
        if (FAILED(device_->CreateTexture2D(&desc, &initial, &texture)) || texture == nullptr) {
            return 0;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;

        ID3D11ShaderResourceView* view = nullptr;
        if (FAILED(device_->CreateShaderResourceView(texture, &srv, &view)) || view == nullptr) {
            texture->Release();
            return 0;
        }

        const std::uint64_t handle = reinterpret_cast<std::uint64_t>(view);
        pairs_[handle] = texture;
        return handle;
    }

    void destroy(std::uint64_t handle) override {
        const auto it = pairs_.find(handle);
        if (it == pairs_.end()) return;
        reinterpret_cast<ID3D11ShaderResourceView*>(handle)->Release();
        it->second->Release();
        pairs_.erase(it);
    }

private:
    ID3D11Device* device_ = nullptr;
    /// view handle -> the texture it views. Both have to be released, and only the view is an
    /// ImTextureID, so the texture is kept here rather than handed back to shared code.
    std::map<std::uint64_t, ID3D11Texture2D*> pairs_;
};

}  // namespace tsro::asi

#endif  // TSRO_D3D11_FONT_SINK_HPP
