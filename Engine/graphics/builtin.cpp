#include "builtin.h"

#include <Tempest/Device>
#include <Tempest/PaintDevice>

#include "builtin_shader.h"

using namespace Tempest;

Builtin::Builtin(Device& device)
  : device(device) {
  static bool internalShaders = true;
  if(internalShaders) {
    brushE  = mkShaderSet(false);
    brushT2 = mkShaderSet(true);
    }
  }

Builtin::Item Builtin::mkShaderSet(bool textures) {
  Tempest::Shader vs, fs;
  if(textures) {
    vs = device.shader(tex_brush_vert_sprv,sizeof(tex_brush_vert_sprv));
    fs = device.shader(tex_brush_frag_sprv,sizeof(tex_brush_frag_sprv));
    } else {
    vs = device.shader(empty_vert_sprv,    sizeof(empty_vert_sprv));
    fs = device.shader(empty_frag_sprv,    sizeof(empty_frag_sprv));
    }

  RenderState stNormal, stBlend, stAlpha;
  stNormal.setZWriteEnabled(false);

  stBlend.setBlendSource  (RenderState::BlendMode::SrcAlpha);
  stBlend.setBlendDest    (RenderState::BlendMode::OneMinusSrcAlpha);
  stBlend.setZWriteEnabled(false);

  stAlpha.setBlendSource  (RenderState::BlendMode::One);
  stAlpha.setBlendDest    (RenderState::BlendMode::One);
  stAlpha.setZWriteEnabled(false);

  Item ret;
  ret.pen    = device.pipeline(Lines,    stNormal,vs,fs);
  ret.brush  = device.pipeline(Triangles,stNormal,vs,fs);

  ret.penB   = device.pipeline(Lines,    stBlend,vs,fs);
  ret.brushB = device.pipeline(Triangles,stBlend,vs,fs);

  ret.penA   = device.pipeline(Lines,    stAlpha,vs,fs);
  ret.brushA = device.pipeline(Triangles,stAlpha,vs,fs);
  return ret;
  }

Builtin::Transmittance::Transmittance(Device& device) {
  const auto solidVs = device.shader(empty_vert_sprv, sizeof(empty_vert_sprv));
  const auto solidFs = device.shader(empty_frag_sprv, sizeof(empty_frag_sprv));
  const auto texVs   = device.shader(tex_brush_vert_sprv, sizeof(tex_brush_vert_sprv));
  const auto texFs   = device.shader(tex_brush_frag_sprv, sizeof(tex_brush_frag_sprv));
  RenderState overwrite;
  overwrite.setZWriteEnabled(false);
  overwrite.setBlendSource(RenderState::BlendMode::Zero);
  overwrite.setBlendDest(RenderState::BlendMode::Zero);
  auto alpha = overwrite;
  alpha.setBlendDest(RenderState::BlendMode::OneMinusSrcAlpha);
  for(size_t i=0; i<2; ++i) {
    const auto topology = i==0 ? Triangles : Lines;
    replace[i]       = device.pipeline(topology,overwrite,solidVs,solidFs);
    solidAlpha[i]    = device.pipeline(topology,alpha,solidVs,solidFs);
    texturedAlpha[i] = device.pipeline(topology,alpha,texVs,texFs);
    }
  }
