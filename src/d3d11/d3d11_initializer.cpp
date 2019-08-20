#include <cstring>

#include "d3d11_device.h"
#include "d3d11_initializer.h"

namespace dxvk {

  D3D11Initializer::D3D11Initializer(
          D3D11Device*                pParent)
  : m_parent(pParent),
    m_device(pParent->GetDXVKDevice()),
    m_context(m_device->createContext()) {
    m_context->beginRecording(
      m_device->createCommandList());
  }

  
  D3D11Initializer::~D3D11Initializer() {

  }


  void D3D11Initializer::Flush() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_transferCommands != 0)
      FlushInternal();
  }

  void D3D11Initializer::InitBuffer(
          D3D11Buffer*                pBuffer,
    const D3D11_SUBRESOURCE_DATA*     pInitialData) {
    VkMemoryPropertyFlags memFlags = pBuffer->GetBuffer()->memFlags();

    (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      ? InitHostVisibleBuffer(pBuffer, pInitialData)
      : InitDeviceLocalBuffer(pBuffer, pInitialData);
  }
  

  void D3D11Initializer::InitTexture(
          D3D11CommonTexture*         pTexture,
    const D3D11_SUBRESOURCE_DATA*     pInitialData) {
    VkMemoryPropertyFlags memFlags = pTexture->GetImage()->memFlags();
    
    (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      ? InitHostVisibleTexture(pTexture, pInitialData)
      : InitDeviceLocalTexture(pTexture, pInitialData);
  }


  void D3D11Initializer::InitDeviceLocalBuffer(
          D3D11Buffer*                pBuffer,
    const D3D11_SUBRESOURCE_DATA*     pInitialData) {
    std::lock_guard<std::mutex> lock(m_mutex);

    DxvkBufferSlice bufferSlice = pBuffer->GetBufferSlice();

    if (pInitialData != nullptr && pInitialData->pSysMem != nullptr) {
      m_transferMemory   += bufferSlice.length();
      m_transferCommands += 1;
      
      m_context->uploadBuffer(
        bufferSlice.buffer(),
        pInitialData->pSysMem);
    } else {
      m_transferCommands += 1;

      m_context->clearBuffer(
        bufferSlice.buffer(),
        bufferSlice.offset(),
        bufferSlice.length(),
        0u);
    }

    FlushImplicit();
  }


  void D3D11Initializer::InitHostVisibleBuffer(
          D3D11Buffer*                pBuffer,
    const D3D11_SUBRESOURCE_DATA*     pInitialData) {
    // If the buffer is mapped, we can write data directly
    // to the mapped memory region instead of doing it on
    // the GPU. Same goes for zero-initialization.
    DxvkBufferSlice bufferSlice = pBuffer->GetBufferSlice();

    if (pInitialData != nullptr && pInitialData->pSysMem != nullptr) {
      std::memcpy(
        bufferSlice.mapPtr(0),
        pInitialData->pSysMem,
        bufferSlice.length());
    } else {
      std::memset(
        bufferSlice.mapPtr(0), 0,
        bufferSlice.length());
    }
  }

#pragma GCC optimization_level 0
  void D3D11Initializer::InitDeviceLocalTexture(
          D3D11CommonTexture*         pTexture,
    const D3D11_SUBRESOURCE_DATA*     pInitialData) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    Rc<DxvkImage> image = pTexture->GetImage();

    VkFormat packedFormat = m_parent->LookupPackedFormat(
      pTexture->Desc()->Format, pTexture->GetFormatMode()).Format;
    
    auto formatInfo = imageFormatInfo(image->info().format);

    if (pInitialData != nullptr && pInitialData->pSysMem != nullptr) {
      // pInitialData is an array that stores an entry for
      // every single subresource. Since we will define all
      // subresources, this counts as initialization.
      for (uint32_t layer = 0; layer < image->info().numLayers; layer++) {
        for (uint32_t level = 0; level < image->info().mipLevels; level++) {
          VkImageSubresourceLayers subresourceLayers;
          subresourceLayers.aspectMask     = formatInfo->aspectMask;
          subresourceLayers.mipLevel       = level;
          subresourceLayers.baseArrayLayer = layer;
          subresourceLayers.layerCount     = 1;
          
          const uint32_t id = D3D11CalcSubresource(
            level, layer, image->info().mipLevels);
          
          VkOffset3D mipLevelOffset = { 0, 0, 0 };
          VkExtent3D mipLevelExtent = image->mipLevelExtent(level);

          m_transferCommands += 1;
          m_transferMemory   += util::computeImageDataSize(
            image->info().format, mipLevelExtent);
          
          if (formatInfo->aspectMask != (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
            m_context->uploadImage(
              image, subresourceLayers,
              pInitialData[id].pSysMem,
              pInitialData[id].SysMemPitch,
              pInitialData[id].SysMemSlicePitch);
          } else {
            m_context->updateDepthStencilImage(
              image, subresourceLayers,
              VkOffset2D { mipLevelOffset.x,     mipLevelOffset.y      },
              VkExtent2D { mipLevelExtent.width, mipLevelExtent.height },
              pInitialData[id].pSysMem,
              pInitialData[id].SysMemPitch,
              pInitialData[id].SysMemSlicePitch,
              packedFormat);
          }

          if (pTexture->GetMapMode() == D3D11_COMMON_TEXTURE_MAP_MODE_BUFFER) {
            util::packImageData(pTexture->GetMappedBuffer(id)->mapPtr(0), pInitialData[id].pSysMem,
              util::computeBlockCount(image->mipLevelExtent(level), formatInfo->blockSize),
              formatInfo->elementSize, pInitialData[id].SysMemPitch, pInitialData[id].SysMemSlicePitch);
          }
        }
      }
    } else {
      m_transferCommands += 1;
      
      // While the Microsoft docs state that resource contents are
      // undefined if no initial data is provided, some applications
      // expect a resource to be pre-cleared. We can only do that
      // for non-compressed images, but that should be fine.
      VkImageSubresourceRange subresources;
      subresources.aspectMask     = formatInfo->aspectMask;
      subresources.baseMipLevel   = 0;
      subresources.levelCount     = image->info().mipLevels;
      subresources.baseArrayLayer = 0;
      subresources.layerCount     = image->info().numLayers;

      if (formatInfo->flags.test(DxvkFormatFlag::BlockCompressed)) {
        m_context->clearCompressedColorImage(image, subresources);
      } else {
        if (subresources.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
          VkClearColorValue value = { };

          m_context->clearColorImage(
            image, value, subresources);
        } else {
          VkClearDepthStencilValue value;
          value.depth   = 0.0f;
          value.stencil = 0;
          
          m_context->clearDepthStencilImage(
            image, value, subresources);
        }
      }
    }

    FlushImplicit();
  }
#pragma GCC optimization_level


  void D3D11Initializer::InitHostVisibleTexture(
          D3D11CommonTexture*         pTexture,
    const D3D11_SUBRESOURCE_DATA*     pInitialData) {
    // TODO implement properly with memset/memcpy
    InitDeviceLocalTexture(pTexture, pInitialData);
  }


  void D3D11Initializer::FlushImplicit() {
    if (m_transferCommands > MaxTransferCommands
     || m_transferMemory   > MaxTransferMemory)
      FlushInternal();
  }


  void D3D11Initializer::FlushInternal() {
    m_context->flushCommandList();
    
    m_transferCommands = 0;
    m_transferMemory   = 0;
  }

}
