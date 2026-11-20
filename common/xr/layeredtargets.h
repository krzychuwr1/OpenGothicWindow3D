#pragma once

#include <Tempest/Attachment>
#include <Tempest/ZBuffer>
#include <Tempest/Device>

#include <cstdint>

// Two-layer render targets (one layer per eye) for multiview rendering in the XR build.
// The images are created here and lent to Tempest, which keeps them in attachment layout.
class LayeredTargets {
  public:
    LayeredTargets() = default;
    ~LayeredTargets();
    LayeredTargets(const LayeredTargets&) = delete;
    LayeredTargets& operator=(const LayeredTargets&) = delete;

    // (Re)creates the targets when the size or formats change; false where unsupported
    bool resize(Tempest::Device& device, uint32_t w, uint32_t h,
                Tempest::TextureFormat diffuse, Tempest::TextureFormat normal, Tempest::TextureFormat depth);
    bool isEmpty() const { return w==0; }

    Tempest::Attachment diffuse, normal;
    Tempest::ZBuffer    depth;

  private:
    void release();

    struct Img { uint64_t image = 0; uint64_t memory = 0; }; // VkImage, VkDeviceMemory
    Tempest::Device*       dev = nullptr;
    Img                    img[3];
    uint32_t               w = 0, h = 0;
    Tempest::TextureFormat frm[3] = {};
  };
